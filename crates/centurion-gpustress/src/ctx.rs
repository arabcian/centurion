//! The logical device and the few Vulkan helpers both test modes use: buffers, compute pipelines,
//! descriptor sets and one fenced submission at a time. Nothing is destroyed: the process ends right
//! after a test and the driver frees everything.
use crate::device::{self, Gpu};
use ash::{ext, vk, Device, Instance};
use std::ffi::CStr;

/// A submission is capped near 500 ms (core) or is one 256 MiB chunk (VRAM); 10 s without the fence is a hung GPU.
const FENCE_TIMEOUT_NS: u64 = 10_000_000_000;

pub struct Ctx {
    pub instance: Instance,
    pub phys: vk::PhysicalDevice,
    pub dev: Device,
    pub mem: vk::PhysicalDeviceMemoryProperties,
    pub limits: vk::PhysicalDeviceLimits,
    pub max_alloc: u64,
    queue: vk::Queue,
    cmd: vk::CommandBuffer,
    fence: vk::Fence,
}

pub enum Mem {
    /// Device-local memory of this heap.
    Device(u32),
    /// Host-visible, coherent, mapped for good: the small parameter / result buffers.
    Host,
}

pub struct Buf {
    pub buf: vk::Buffer,
    pub size: u64,
    ptr: *mut u32,  // null unless Mem::Host
}

impl Buf {
    pub fn get(&self, i: usize) -> u32 {
        assert!(!self.ptr.is_null() && (i as u64) * 4 < self.size);
        // SAFETY: inside a mapped, coherent allocation; the device is idle (every submit waits).
        unsafe { self.ptr.add(i).read_volatile() }
    }
    pub fn set(&self, i: usize, v: u32) {
        assert!(!self.ptr.is_null() && (i as u64) * 4 < self.size);
        // SAFETY: as in `get`.
        unsafe { self.ptr.add(i).write_volatile(v) }
    }
    /// The shaders' result layout: the count of wrong words, then the first 16 as (index, expected, actual).
    /// Widened to u64 so the VRAM test can put the full word index in the index slot.
    pub fn mismatches(&self) -> (u32, Vec<[u64; 3]>) {
        let n = self.get(0);
        (n, (0..n.min(16) as usize).map(|k| [self.get(1 + 3 * k) as u64, self.get(2 + 3 * k) as u64, self.get(3 + 3 * k) as u64]).collect())
    }
}

impl Ctx {
    pub fn new(instance: &Instance, gpu: &Gpu) -> Result<Ctx, vk::Result> {
        // SAFETY: every handle below comes from this instance / device and is used on one thread.
        unsafe {
            let fams = instance.get_physical_device_queue_family_properties(gpu.phys);
            let fam = fams.iter().position(|f| f.queue_flags.contains(vk::QueueFlags::COMPUTE))
                .ok_or(vk::Result::ERROR_FEATURE_NOT_PRESENT)? as u32;
            let prio = [1.0f32];
            let q = [vk::DeviceQueueCreateInfo::default().queue_family_index(fam).queue_priorities(&prio)];
            let dev = instance.create_device(gpu.phys, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None)?;
            let pool = dev.create_command_pool(&vk::CommandPoolCreateInfo::default()
                .queue_family_index(fam).flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER), None)?;
            let cmd = dev.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(pool).command_buffer_count(1))?[0];
            let fence = dev.create_fence(&vk::FenceCreateInfo::default(), None)?;
            let mut m3 = vk::PhysicalDeviceMaintenance3Properties::default();
            let mut p2 = vk::PhysicalDeviceProperties2::default().push_next(&mut m3);
            instance.get_physical_device_properties2(gpu.phys, &mut p2);
            Ok(Ctx {
                instance: instance.clone(), phys: gpu.phys, queue: dev.get_device_queue(fam, 0), dev, cmd, fence,
                mem: instance.get_physical_device_memory_properties(gpu.phys),
                limits: p2.properties.limits, max_alloc: m3.max_memory_allocation_size,
            })
        }
    }

    /// The device-local heap (the biggest, when the driver exposes several) and what this process may use of it.
    pub fn heap_budget(&self) -> (u32, u64) {
        let heaps = &self.mem.memory_heaps[..self.mem.memory_heap_count as usize];
        let (heap, size) = heaps.iter().enumerate().filter(|(_, h)| h.flags.contains(vk::MemoryHeapFlags::DEVICE_LOCAL))
            .max_by_key(|(_, h)| h.size).map_or((0, heaps[0].size), |(i, h)| (i, h.size));
        let mut budget = size;
        if device::has_ext(&self.instance, self.phys, ext::memory_budget::NAME) {
            let mut b = vk::PhysicalDeviceMemoryBudgetPropertiesEXT::default();
            {
                let mut p2 = vk::PhysicalDeviceMemoryProperties2::default().push_next(&mut b);
                // SAFETY: the extension is supported by this device.
                unsafe { self.instance.get_physical_device_memory_properties2(self.phys, &mut p2) };
            }
            if b.heap_budget[heap] > 0 { budget = b.heap_budget[heap]; }
        }
        (heap as u32, budget)
    }

    pub fn buffer(&self, size: u64, usage: vk::BufferUsageFlags, mem: Mem) -> Result<Buf, vk::Result> {
        let types = &self.mem.memory_types[..self.mem.memory_type_count as usize];
        // SAFETY: plain create / allocate / bind / map calls on handles made here.
        unsafe {
            let buf = self.dev.create_buffer(&vk::BufferCreateInfo::default().size(size).usage(usage).sharing_mode(vk::SharingMode::EXCLUSIVE), None)?;
            let bits = self.dev.get_buffer_memory_requirements(buf).memory_type_bits;
            let ok = |f: vk::MemoryPropertyFlags, i: usize| (bits >> i) & 1 == 1 && match mem {
                Mem::Device(heap) => f.contains(vk::MemoryPropertyFlags::DEVICE_LOCAL) && types[i].heap_index == heap,
                Mem::Host => f.contains(vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT),
            };
            // Device: a type that is not also host-visible, so a resizable-BAR window is not what gets filled.
            // Host: plain system memory rather than the BAR, which is slow to read from the host.
            let avoid = match mem { Mem::Device(_) => vk::MemoryPropertyFlags::HOST_VISIBLE, Mem::Host => vk::MemoryPropertyFlags::DEVICE_LOCAL };
            let Some(ty) = (0..types.len()).filter(|&i| ok(types[i].property_flags, i)).min_by_key(|&i| types[i].property_flags.contains(avoid)) else {
                self.dev.destroy_buffer(buf, None);
                return Err(match mem { Mem::Device(_) => vk::Result::ERROR_OUT_OF_DEVICE_MEMORY, Mem::Host => vk::Result::ERROR_OUT_OF_HOST_MEMORY });
            };
            let req = self.dev.get_buffer_memory_requirements(buf);
            let memory = match self.dev.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(ty as u32), None) {
                Ok(m) => m,
                Err(e) => { self.dev.destroy_buffer(buf, None); return Err(e); }
            };
            self.dev.bind_buffer_memory(buf, memory, 0)?;
            let ptr = match mem {
                Mem::Device(_) => std::ptr::null_mut(),
                Mem::Host => self.dev.map_memory(memory, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty())? as *mut u32,
            };
            Ok(Buf { buf, size, ptr })
        }
    }

    /// Descriptor set layout + pipeline layout for one binding per entry of `kinds`.
    pub fn layout(&self, kinds: &[vk::DescriptorType]) -> Result<(vk::DescriptorSetLayout, vk::PipelineLayout), vk::Result> {
        let binds: Vec<_> = kinds.iter().enumerate().map(|(i, &k)| vk::DescriptorSetLayoutBinding::default()
            .binding(i as u32).descriptor_type(k).descriptor_count(1).stage_flags(vk::ShaderStageFlags::COMPUTE)).collect();
        // SAFETY: see `new`.
        unsafe {
            let dsl = self.dev.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&binds), None)?;
            let layouts = [dsl];
            Ok((dsl, self.dev.create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&layouts), None)?))
        }
    }

    /// `count` descriptor sets of `layout`; `kinds` is the layout's binding list.
    pub fn sets(&self, layout: vk::DescriptorSetLayout, kinds: &[vk::DescriptorType], count: u32) -> Result<Vec<vk::DescriptorSet>, vk::Result> {
        let sizes: Vec<_> = kinds.iter().map(|&k| vk::DescriptorPoolSize::default().ty(k).descriptor_count(count)).collect();
        let layouts = vec![layout; count as usize];
        // SAFETY: see `new`.
        unsafe {
            let pool = self.dev.create_descriptor_pool(&vk::DescriptorPoolCreateInfo::default().max_sets(count).pool_sizes(&sizes), None)?;
            self.dev.allocate_descriptor_sets(&vk::DescriptorSetAllocateInfo::default().descriptor_pool(pool).set_layouts(&layouts))
        }
    }

    /// Points binding i of `set` at `bufs[i]`.
    pub fn bind(&self, set: vk::DescriptorSet, bufs: &[(vk::DescriptorType, &Buf)]) {
        for (i, (kind, b)) in bufs.iter().enumerate() {
            let info = [vk::DescriptorBufferInfo::default().buffer(b.buf).range(vk::WHOLE_SIZE)];
            let w = [vk::WriteDescriptorSet::default().dst_set(set).dst_binding(i as u32).descriptor_type(*kind).buffer_info(&info)];
            // SAFETY: the set is not in use (every submit waits for its fence).
            unsafe { self.dev.update_descriptor_sets(&w, &[]) };
        }
    }

    pub fn module(&self, spv: &[u8]) -> Result<vk::ShaderModule, vk::Result> {
        let words: Vec<u32> = spv.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        // SAFETY: see `new`; the SPIR-V comes from build.rs.
        unsafe { self.dev.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None) }
    }

    pub fn pipeline(&self, layout: vk::PipelineLayout, module: vk::ShaderModule, entry: &CStr) -> Result<vk::Pipeline, vk::Result> {
        let stage = vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::COMPUTE).module(module).name(entry);
        let info = [vk::ComputePipelineCreateInfo::default().stage(stage).layout(layout)];
        // SAFETY: see `new`.
        unsafe { self.dev.create_compute_pipelines(vk::PipelineCache::null(), &info, None) }.map(|p| p[0]).map_err(|(_, e)| e)
    }

    /// Records with `record`, submits and waits for the fence: one submission at a time, so none comes
    /// near a driver watchdog and the host may touch the small buffers between submissions.
    pub fn submit(&self, record: impl FnOnce(&Device, vk::CommandBuffer)) -> Result<(), vk::Result> {
        let d = &self.dev;
        let rw = vk::AccessFlags::SHADER_READ | vk::AccessFlags::SHADER_WRITE;
        // SAFETY: see `new`; the command buffer is idle because the previous submit waited.
        unsafe {
            d.begin_command_buffer(self.cmd, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT))?;
            // The fence only orders a submission for the host: what an earlier one wrote must also be
            // made visible to this one on the device.
            let before = [vk::MemoryBarrier::default().src_access_mask(rw).dst_access_mask(rw)];
            d.cmd_pipeline_barrier(self.cmd, vk::PipelineStageFlags::COMPUTE_SHADER, vk::PipelineStageFlags::COMPUTE_SHADER,
                vk::DependencyFlags::empty(), &before, &[], &[]);
            record(d, self.cmd);
            let after = [vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::SHADER_WRITE).dst_access_mask(vk::AccessFlags::HOST_READ)];
            d.cmd_pipeline_barrier(self.cmd, vk::PipelineStageFlags::COMPUTE_SHADER, vk::PipelineStageFlags::HOST,
                vk::DependencyFlags::empty(), &after, &[], &[]);
            d.end_command_buffer(self.cmd)?;
            d.reset_fences(&[self.fence])?;
            let cmds = [self.cmd];
            d.queue_submit(self.queue, &[vk::SubmitInfo::default().command_buffers(&cmds)], self.fence)?;
            d.wait_for_fences(&[self.fence], true, FENCE_TIMEOUT_NS)
        }
    }
}

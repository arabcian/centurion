//! Vulkan instance and device discovery. Devices are told apart by PCI address, never "the first one":
//! a laptop has an iGPU and a dGPU.
use ash::{vk, Entry, Instance};
use std::ffi::CStr;
use std::fs;

pub struct Gpu {
    pub phys: vk::PhysicalDevice,
    /// PCI address as in sysfs; "vk:<n>" (n = Vulkan enumeration index) when it cannot be determined,
    /// which is the normal case for CPU implementations such as lavapipe.
    pub pci: String,
    pub name: String,
    /// "discrete", "integrated", "cpu" or "other".
    pub kind: &'static str,
    pub vendor_id: u32,
    pub device_id: u32,
    pub vram_mib: u64,
    pub driver: String,
}

pub fn open() -> Result<(Entry, Instance), String> {
    // SAFETY: loads libvulkan.so.1 at run time; a missing library is an ordinary error.
    let entry = unsafe { Entry::load() }.map_err(|e| format!("Vulkan loader not found: {e}"))?;
    let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_1);
    let info = vk::InstanceCreateInfo::default().application_info(&app);
    // SAFETY: valid create info, no extensions or layers.
    let instance = unsafe { entry.create_instance(&info, None) }.map_err(|e| format!("could not create a Vulkan instance: {e}"))?;
    Ok((entry, instance))
}

pub fn has_ext(instance: &Instance, phys: vk::PhysicalDevice, name: &CStr) -> bool {
    // SAFETY: phys comes from this instance.
    unsafe { instance.enumerate_device_extension_properties(phys) }
        .map(|v| v.iter().any(|e| e.extension_name_as_c_str().is_ok_and(|n| n == name)))
        .unwrap_or(false)
}

fn hex_attr(dir: &std::path::Path, name: &str) -> Option<u32> {
    let s = fs::read_to_string(dir.join(name)).ok()?;
    u32::from_str_radix(s.trim().trim_start_matches("0x"), 16).ok()
}

/// Fallback when the driver lacks VK_EXT_pci_bus_info: the sysfs device with these IDs, only if unique.
fn sysfs_pci(vendor: u32, device: u32) -> Option<String> {
    let mut found = None;
    for e in fs::read_dir("/sys/bus/pci/devices").ok()?.flatten() {
        if hex_attr(&e.path(), "vendor") == Some(vendor) && hex_attr(&e.path(), "device") == Some(device) {
            if found.is_some() { return None; }  // two cards of one model: cannot tell which is which
            found = Some(e.file_name().to_string_lossy().into_owned());
        }
    }
    found
}

pub fn list(instance: &Instance) -> Result<Vec<Gpu>, String> {
    // SAFETY: valid instance.
    let physs = unsafe { instance.enumerate_physical_devices() }.map_err(|e| format!("could not enumerate devices: {e}"))?;
    let mut out = Vec::new();
    for (i, &phys) in physs.iter().enumerate() {
        // SAFETY: phys comes from this instance.
        let props = unsafe { instance.get_physical_device_properties(phys) };
        if props.api_version < vk::API_VERSION_1_1 { continue; }  // properties2 / memory budget queries below need 1.1
        let mut pci = None;
        if has_ext(instance, phys, ash::ext::pci_bus_info::NAME) {
            let mut b = vk::PhysicalDevicePCIBusInfoPropertiesEXT::default();
            let mut p2 = vk::PhysicalDeviceProperties2::default().push_next(&mut b);
            // SAFETY: the extension is supported by this device.
            unsafe { instance.get_physical_device_properties2(phys, &mut p2) };
            pci = Some(format!("{:04x}:{:02x}:{:02x}.{:x}", b.pci_domain, b.pci_bus, b.pci_device, b.pci_function));
        }
        let pci = pci.or_else(|| sysfs_pci(props.vendor_id, props.device_id)).unwrap_or_else(|| format!("vk:{i}"));
        let mut driver = String::new();
        if has_ext(instance, phys, ash::khr::driver_properties::NAME) || props.api_version >= vk::API_VERSION_1_2 {
            let mut d = vk::PhysicalDeviceDriverProperties::default();
            let mut p2 = vk::PhysicalDeviceProperties2::default().push_next(&mut d);
            // SAFETY: driver properties are core in 1.2 or supported through the extension (checked above).
            unsafe { instance.get_physical_device_properties2(phys, &mut p2) };
            let n = d.driver_name_as_c_str().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
            let info = d.driver_info_as_c_str().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
            driver = if info.is_empty() { n } else { format!("{n} {info}") };
        }
        // SAFETY: phys comes from this instance.
        let mem = unsafe { instance.get_physical_device_memory_properties(phys) };
        let vram = mem.memory_heaps[..mem.memory_heap_count as usize].iter()
            .filter(|h| h.flags.contains(vk::MemoryHeapFlags::DEVICE_LOCAL)).map(|h| h.size).max().unwrap_or(0);
        out.push(Gpu {
            phys, pci,
            name: props.device_name_as_c_str().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default(),
            kind: match props.device_type {
                vk::PhysicalDeviceType::DISCRETE_GPU => "discrete",
                vk::PhysicalDeviceType::INTEGRATED_GPU => "integrated",
                vk::PhysicalDeviceType::CPU => "cpu",
                _ => "other",
            },
            vendor_id: props.vendor_id, device_id: props.device_id, vram_mib: vram >> 20, driver,
        });
    }
    Ok(out)
}

/// The device the user named. Never "the first one": a laptop has an iGPU and a dGPU.
pub fn find<'a>(gpus: &'a [Gpu], pci: &str) -> Result<&'a Gpu, String> {
    gpus.iter().find(|g| g.pci.eq_ignore_ascii_case(pci)).ok_or_else(|| format!("no Vulkan device {pci} (see `centurion-gpustress list`)"))
}

#pragma once
// ACPI generic-netlink notification wire format. No sockets or Qt in this parser.
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
namespace acpievents {
struct Event { std::string deviceClass, busId; uint32_t type=0,data=0,sequence=0; };
namespace wire {
struct Payload { char deviceClass[20]; char busId[15]; uint32_t type; uint32_t data; };
static_assert(sizeof(Payload)==44);
inline std::optional<std::string> text(const char* p,std::size_t size) {
    const auto len=strnlen(p,size);
    if(len==size)return {};
    for(std::size_t i=0;i<len;++i)if(static_cast<unsigned char>(p[i])<32||static_cast<unsigned char>(p[i])==127)return {};
    return std::string(p,len);
}
}
// Reject malformed datagrams atomically rather than dispatching a valid prefix.
inline std::optional<std::vector<Event>> parseChecked(const void* buf,std::size_t len,uint16_t family) {
    if(!buf||len==0||len>8192||family<GENL_MIN_ID)return {};
    const auto* p=static_cast<const char*>(buf);std::vector<Event> out;
    while(len){
        if(len<NLMSG_HDRLEN)return {};
        nlmsghdr h{};std::memcpy(&h,p,sizeof(h));
        if(h.nlmsg_len<NLMSG_HDRLEN||h.nlmsg_len>len)return {};
        if(h.nlmsg_type==family){
            if(h.nlmsg_len<NLMSG_HDRLEN+GENL_HDRLEN)return {};
            genlmsghdr g{};std::memcpy(&g,p+NLMSG_HDRLEN,sizeof(g));
            if(g.cmd!=1||g.version!=1)return {};
            const char* attr=p+NLMSG_HDRLEN+GENL_HDRLEN;std::size_t left=h.nlmsg_len-NLMSG_HDRLEN-GENL_HDRLEN;
            std::optional<Event> event;
            while(left){
                if(left<NLA_HDRLEN)return {};
                nlattr a{};std::memcpy(&a,attr,sizeof(a));
                if(a.nla_len<NLA_HDRLEN||a.nla_len>left)return {};
                if((a.nla_type & NLA_TYPE_MASK)==1){
                    if(event||a.nla_len!=NLA_HDRLEN+sizeof(wire::Payload))return {};
                    wire::Payload e{};std::memcpy(&e,attr+NLA_HDRLEN,sizeof(e));
                    auto cls=wire::text(e.deviceClass,sizeof(e.deviceClass)),id=wire::text(e.busId,sizeof(e.busId));
                    if(!cls||!id)return {};
                    event=Event{*cls,*id,e.type,e.data,h.nlmsg_seq};
                }
                const std::size_t step=NLA_ALIGN(a.nla_len);
                if(step>left){if(a.nla_len!=left)return {};left=0;}else{attr+=step;left-=step;}
            }
            if(!event)return {};
            out.push_back(*event);
        } else if(h.nlmsg_type==NLMSG_OVERRUN||h.nlmsg_type==NLMSG_ERROR)return {};
        const std::size_t step=NLMSG_ALIGN(h.nlmsg_len);
        if(step>len){if(h.nlmsg_len!=len)return {};len=0;}else{p+=step;len-=step;}
    }
    return out;
}
inline std::vector<Event> parse(const void* buf,std::size_t len,uint16_t family){
    auto checked=parseChecked(buf,len,family);return checked?*checked:std::vector<Event>{};
}
}

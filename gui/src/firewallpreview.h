#pragma once
// Read-only model of the saved Centurion firewall's ordered local NEW TCP/UDP chain.
// Does not predict the effective nftables ruleset, guard, NAT or conntrack.
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace firewallpreview {
inline std::string_view trim(std::string_view s) { auto p=s.find_first_not_of(" \t\r\n"); return p==s.npos?std::string_view{}:s.substr(p,s.find_last_not_of(" \t\r\n")-p+1); }
inline std::optional<unsigned> number(std::string_view s) { s=trim(s); if(s.empty())return {}; unsigned n=0; auto r=std::from_chars(s.data(),s.data()+s.size(),n); if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size())return {}; return n; }
struct Address { int family=0; std::array<unsigned char,16> bytes{}; auto operator<=>(const Address&) const=default; };
inline std::optional<Address> address(std::string_view s) { s=trim(s); if(s.empty()||s.size()>64||s.find('\0')!=s.npos)return {}; std::string z(s); Address a; if(inet_pton(AF_INET,z.c_str(),a.bytes.data())==1){a.family=4;return a;} if(inet_pton(AF_INET6,z.c_str(),a.bytes.data())!=1)return {}; a.family=6; // Mapped IPv6 socket addresses do not identify the actual packet family.
 if(std::all_of(a.bytes.begin(),a.bytes.begin()+10,[](auto x){return x==0;})&&a.bytes[10]==255&&a.bytes[11]==255)return {};
 return a; }
struct Range { Address lo,hi; auto operator<=>(const Range&) const=default; };
inline std::optional<Range> range(std::string_view s) { auto d=s.find('-'),p=s.find('/'); if(d!=s.npos){if(p!=s.npos)return {};auto a=address(s.substr(0,d)),b=address(s.substr(d+1));if(!a||!b||a->family!=b->family||*b<*a)return {};return Range{*a,*b};} auto a=address(s.substr(0,p));if(!a)return {};unsigned bits=a->family==4?32:128; auto n=p==s.npos?std::optional<unsigned>(bits):number(s.substr(p+1));if(!n||!*n||*n>bits)return {};Range r{*a,*a};for(unsigned i=*n;i<bits;++i){r.lo.bytes[i/8]&=static_cast<unsigned char>(~(128u>>(i%8)));r.hi.bytes[i/8]|=static_cast<unsigned char>(128u>>(i%8));}return r; }
inline bool contains(const Range&r,const Address&a){return a.family==r.lo.family&&r.lo<=a&&a<=r.hi;}
inline bool adjacent(Address a,const Address& b){if(a.family!=b.family)return false;for(int i=(a.family==4?4:16)-1;i>=0;--i){if(++a.bytes[i]!=0)return a==b;}return false;}
template<class F> bool items(std::string_view s,unsigned limit,F f){if(s.size()>16384)return false;unsigned count=0;while(true){auto p=s.find(',');auto t=trim(s.substr(0,p));if(!t.empty()&&(++count>limit||!f(t)))return false;if(p==s.npos)return true;s.remove_prefix(p+1);}}
using Ports=std::vector<std::pair<unsigned,unsigned>>;
inline std::optional<Ports> ports(std::string_view s){Ports v;if(!items(s,32,[&](auto t){auto p=t.find('-');auto a=number(t.substr(0,p)),b=p==t.npos?a:number(t.substr(p+1));if(!a||!b||!*a||*b>65535||*a>*b)return false;v.emplace_back(*a,*b);return true;}))return {};std::sort(v.begin(),v.end());Ports out;for(auto r:v){if(!out.empty()&&r.first<=out.back().second+1)out.back().second=std::max(r.second,out.back().second);else out.push_back(r);}return out;}
struct Remote { enum Kind { Any,Lan,Nets } kind=Any; std::vector<Range> ranges; bool operator==(const Remote&)const=default; };
inline std::optional<Remote> remote(std::string_view s){s=trim(s);Remote r;if(s.empty()||s=="any")return r;if(s=="lan"){r.kind=Remote::Lan;return r;}r.kind=Remote::Nets;if(!items(s,64,[&](auto t){auto x=range(t);if(!x)return false;r.ranges.push_back(*x);return true;})||r.ranges.empty())return {};std::sort(r.ranges.begin(),r.ranges.end());std::vector<Range> merged;for(auto x:r.ranges){if(!merged.empty()&&x.lo.family==merged.back().lo.family&&(x.lo<=merged.back().hi||adjacent(merged.back().hi,x.lo)))merged.back().hi=std::max(merged.back().hi,x.hi);else merged.push_back(x);}r.ranges=std::move(merged);return r;}
inline bool in(const Address&a,std::string_view cidr){auto r=range(cidr);return r&&contains(*r,a);}
inline bool matches(const Remote&r,const Address&a,bool inbound){if(r.kind==Remote::Any)return true;if(r.kind==Remote::Nets)return std::any_of(r.ranges.begin(),r.ranges.end(),[&](auto x){return contains(x,a);});for(auto n:{"10.0.0.0/8","169.254.0.0/16","172.16.0.0/12","192.168.0.0/16","fc00::/7","fe80::/10"})if(in(a,n))return true;return !inbound&&(in(a,"224.0.0.0/4")||in(a,"255.255.255.255")||in(a,"ff00::/8"));}
enum class Verdict { Unknown,Disabled,Allow,Block,Reject };
inline Verdict verdict(std::string_view s){return s=="allow"?Verdict::Allow:s=="block"?Verdict::Block:s=="reject"?Verdict::Reject:Verdict::Unknown;}
inline bool ifaceValid(std::string_view s){if(s.empty())return true;if(s.size()>15)return false;if(s.back()=='*')s.remove_suffix(1);return !s.empty()&&s!="lo"&&std::all_of(s.begin(),s.end(),[](char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';});}
struct Rule{uint32_t id=0;bool enabled=false,inbound=false;Verdict action=Verdict::Unknown;std::string proto,iface;Ports ports;Remote remote;};
struct Config{bool enabled=false;Verdict inbound=Verdict::Unknown,outbound=Verdict::Unknown;std::vector<Rule> rules;};
struct Packet{bool inbound=false;std::string proto,iface;Address peer;unsigned destinationPort=0;};
struct Result{Verdict value=Verdict::Unknown;uint32_t rule=0;};
inline Result evaluate(const Config&c,const Packet&p){if(c.inbound==Verdict::Unknown||c.outbound==Verdict::Unknown||c.rules.size()>512||(p.proto!="tcp"&&p.proto!="udp")||!p.destinationPort||p.destinationPort>65535||!ifaceValid(p.iface)||p.iface.find('*')!=std::string::npos)return {};if(!c.enabled)return {Verdict::Disabled,0};if(in(p.peer,"127.0.0.0/8")||in(p.peer,"::1")||(p.proto=="udp"&&(p.destinationPort==67||p.destinationPort==68||p.destinationPort==546||p.destinationPort==547)))return {};for(const auto&r:c.rules){if(!r.enabled||r.inbound!=p.inbound||(r.proto!="any"&&r.proto!=p.proto)||!matches(r.remote,p.peer,p.inbound))continue;if(!r.ports.empty()&&std::none_of(r.ports.begin(),r.ports.end(),[&](auto x){return x.first<=p.destinationPort&&p.destinationPort<=x.second;}))continue;if(!r.iface.empty()){if(p.iface.empty())return {};auto prefix=std::string_view(r.iface);if(prefix.back()=='*'){prefix.remove_suffix(1);if(!std::string_view(p.iface).starts_with(prefix))continue;}else if(p.iface!=r.iface)continue;}return {r.action,r.id};}return {p.inbound?c.inbound:c.outbound,0};}
}

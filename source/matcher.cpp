#include "matcher.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <utility>

namespace mtga { namespace {
using U8 = std::uint8_t;
using U32 = std::uint32_t;
using U64 = std::uint64_t;
using Size = std::size_t;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Bytes {
    const std::vector<U8>& data;
    void check(Size p, Size n) const {
        require(p <= data.size() && n <= data.size() - p, "Truncated or out-of-range PE/CLI data.");
    }
    U8 u8(Size p) const { check(p, 1); return data[p]; }
    std::uint16_t u16(Size p) const {
        check(p, 2); return static_cast<std::uint16_t>(data[p] | (U32(data[p+1]) << 8));
    }
    U32 u32(Size p) const {
        check(p, 4);
        return U32(data[p]) | (U32(data[p+1]) << 8) | (U32(data[p+2]) << 16) | (U32(data[p+3]) << 24);
    }
    U64 u64(Size p) const { return U64(u32(p)) | (U64(u32(p+4)) << 32); }
    U32 index(Size p, Size width) const { return width == 2 ? u16(p) : u32(p); }
};
struct Range { Size start = 0, size = 0; };
struct Section { U32 rva, raw_size, raw_start; };
struct Instruction {
    U32 offset = 0, operand_offset = 0, operand = 0;
    unsigned op = 0;
    std::int64_t branch = -1;
};
struct Body {
    Size code_offset = 0;
    U32 local_token = 0, max_stack = 0;
    std::vector<Instruction> all, code;
    Size branch_target(const Instruction& ins) const {
        require(ins.branch >= 0, "Instruction is not a valid branch.");
        auto p = std::find_if(all.begin(), all.end(), [&](const Instruction& x){return x.offset == ins.branch;});
        require(p != all.end(), "IL branch does not target an instruction boundary.");
        while (p != all.end() && p->op == 0) ++p;
        require(p != all.end(), "IL branch targets beyond executable instructions.");
        auto q = std::find_if(code.begin(), code.end(), [&](const Instruction& x){return x.offset == p->offset;});
        require(q != code.end(), "IL branch mapping failed.");
        return static_cast<Size>(q - code.begin());
    }
};
// ECMA-335 III.1.5 operand encodings. Unknown/reserved instructions fail closed.
int operand_size(unsigned op) {
    if (op <= 0x0d || op == 0x14 || (op >= 0x15 && op <= 0x1e) || op == 0x25 || op == 0x26 || op == 0x2a
        || (op >= 0x46 && op <= 0x6e) || op == 0x76 || op == 0x7a || (op >= 0x82 && op <= 0x8b)
        || op == 0x8e || (op >= 0x90 && op <= 0xa2) || (op >= 0xb3 && op <= 0xba) || op == 0xc3
        || (op >= 0xd1 && op <= 0xdc) || op == 0xdf || op == 0xe0 || (op >= 0xfe00 && op <= 0xfe05)
        || op == 0xfe0f || op == 0xfe11 || op == 0xfe13 || op == 0xfe14 || op == 0xfe17 || op == 0xfe18
        || op == 0xfe1a || op == 0xfe1d || op == 0xfe1e) return 0;
    if ((op >= 0x0e && op <= 0x13) || op == 0x1f || (op >= 0x2b && op <= 0x37) || op == 0xde || op == 0xfe12) return 1;
    if (op >= 0xfe09 && op <= 0xfe0e) return 2;
    if (op == 0x20 || op == 0x22 || (op >= 0x27 && op <= 0x29) || (op >= 0x38 && op <= 0x44)
        || (op >= 0x6f && op <= 0x75) || op == 0x79 || (op >= 0x7b && op <= 0x81) || op == 0x8c
        || op == 0x8d || op == 0x8f || (op >= 0xa3 && op <= 0xa5) || op == 0xc2 || op == 0xc6 || op == 0xd0
        || op == 0xdd || op == 0xfe06 || op == 0xfe07 || op == 0xfe15 || op == 0xfe16 || op == 0xfe1c) return 4;
    if (op == 0x21 || op == 0x23) return 8;
    if (op == 0x45) return -1;
    throw std::runtime_error("Unknown or reserved IL opcode.");
}
bool additional_flow(unsigned op) {
    return (op >= 0x2b && op <= 0x45) || op == 0x2a || op == 0x7a || op == 0xdc
        || op == 0xdd || op == 0xde || op == 0xfe11 || op == 0xfe1a;
}
float as_float(U32 n) { float f; static_assert(sizeof(f) == 4, "IEEE 754 binary32 required"); std::memcpy(&f, &n, 4); return f; }
class Parser {
    Bytes b;
    std::vector<Section> sections;
    Range strings, blobs, tables;
    std::array<U32, 64> rows{};
    std::array<Size, 64> starts{}, row_sizes{};
    Size sw = 2, bw = 2, gw = 2;
    U32 cor_flags = 0, native_rva = 0, native_size = 0;
    MatchResult& result;
    Size ix(unsigned table) const { return rows[table] < 65536 ? 2 : 4; }
    Size coded(unsigned bits, std::initializer_list<unsigned> list) const {
        U32 maximum = 0; for (unsigned t : list) maximum = std::max(maximum, rows[t]);
        return maximum < (U32(1) << (16-bits)) ? 2 : 4;
    }
    Size tdorref() const { return coded(2, {2,1,27}); }
    Size typeormethod() const { return coded(1, {2,6}); }
    Size methodorref() const { return coded(1, {6,10}); }
    Size row(unsigned t, U32 id) const {
        require(t < 64 && id >= 1 && id <= rows[t], "Invalid CLI table row reference.");
        return starts[t] + Size(id-1) * row_sizes[t];
    }
    Size map(U32 rva, Size length) const {
        Size found = 0, offset = 0;
        for (const auto& s : sections) {
            if (rva >= s.rva && U64(rva)-s.rva < s.raw_size && length <= U64(s.raw_size)-(U64(rva)-s.rva)) {
                offset = Size(s.raw_start) + (rva - s.rva); ++found;
            }
        }
        require(found == 1, "RVA does not map to exactly one complete file section.");
        b.check(offset, length); return offset;
    }
    std::string str(U32 index) const {
        require(index < strings.size, "String heap index is out of range.");
        Size p = strings.start + index, end = strings.start + strings.size;
        Size q = p; while (q < end && b.data[q] != 0) ++q;
        require(q < end, "Unterminated CLI string.");
        return std::string(b.data.data()+p, b.data.data()+q);
    }
    std::vector<U8> blob(U32 index) const {
        require(index < blobs.size, "Blob heap index is out of range.");
        Size p = blobs.start + index, end = blobs.start + blobs.size;
        U32 first = b.u8(p++), n = 0;
        if ((first & 0x80) == 0) n = first;
        else if ((first & 0xc0) == 0x80) {
            require(p < end, "Truncated compressed blob length."); n = ((first & 0x3f) << 8) | b.u8(p++);
        } else {
            require((first & 0xe0) == 0xc0 && end-p >= 3, "Invalid compressed blob length.");
            n = ((first & 0x1f) << 24) | (U32(b.u8(p)) << 16) | (U32(b.u8(p+1)) << 8) | b.u8(p+2); p += 3;
        }
        require(n <= end-p, "CLI blob extends beyond its heap.");
        return std::vector<U8>(b.data.data()+p, b.data.data()+p+n);
    }
    bool signature(U32 index, std::initializer_list<U8> expected) const {
        return blob(index) == std::vector<U8>(expected);
    }
    Size table_size(unsigned t) const {
        switch(t) {
        case 0: return 2+sw+3*gw;
        case 1: return coded(2,{0,26,35,1})+2*sw;
        case 2: return 4+2*sw+tdorref()+ix(4)+ix(6);
        case 3: return ix(4);
        case 4: return 2+sw+bw;
        case 5: return ix(6);
        case 6: return 8+sw+bw+ix(8);
        case 7: return ix(8);
        case 8: return 4+sw;
        case 9: return ix(2)+tdorref();
        case 10: return coded(3,{2,1,26,6,27})+sw+bw;
        case 11: return 2+coded(2,{4,8,23})+bw;
        case 12: return coded(5,{6,4,1,2,8,9,10,0,14,23,20,17,26,27,32,35,38,39,40,42,44,43})+coded(3,{6,10})+bw;
        case 13: return coded(1,{4,8})+bw;
        case 14: return 2+coded(2,{2,6,32})+bw;
        case 15: return 6+ix(2);
        case 16: return 4+ix(4);
        case 17: return bw;
        case 18: return ix(2)+ix(20);
        case 19: return ix(20);
        case 20: return 2+sw+tdorref();
        case 21: return ix(2)+ix(23);
        case 22: return ix(23);
        case 23: return 2+sw+bw;
        case 24: return 2+ix(6)+coded(1,{20,23});
        case 25: return ix(2)+2*methodorref();
        case 26: return sw;
        case 27: return bw;
        case 28: return 2+coded(1,{4,6})+sw+ix(26);
        case 29: return 4+ix(4);
        case 30: return 8;
        case 31: return 4;
        case 32: return 16+bw+2*sw;
        case 33: return 4;
        case 34: return 12;
        case 35: return 12+2*bw+2*sw;
        case 36: return 4+ix(35);
        case 37: return 12+ix(35);
        case 38: return 4+sw+bw;
        case 39: return 8+2*sw+coded(2,{38,35,39});
        case 40: return 8+sw+coded(2,{38,35,39});
        case 41: return 2*ix(2);
        case 42: return 4+typeormethod()+sw;
        case 43: return methodorref()+bw;
        case 44: return ix(42)+tdorref();
        default: throw std::runtime_error("Unsupported CLI metadata table.");
        }
    }
    void read_tables() {
        require(tables.size >= 24, "Truncated CLI tables stream.");
        Size p = tables.start; U8 heaps = b.u8(p+6);
        // Extra-data/EnC/deleted-row stream extensions are not patch targets.
        require((heaps & ~7) == 0, "Unsupported CLI metadata stream extensions.");
        sw = heaps & 1 ? 4 : 2; gw = heaps & 2 ? 4 : 2; bw = heaps & 4 ? 4 : 2;
        U64 valid = b.u64(p+8); p += 24;
        for (unsigned t=0;t<64;++t) if (valid & (U64(1) << t)) {
            require(p <= tables.start+tables.size && 4 <= tables.start+tables.size-p, "Truncated CLI table counts.");
            rows[t] = b.u32(p); p += 4;
        }
        require(rows[3] == 0 && rows[5] == 0 && rows[7] == 0 && rows[19] == 0 && rows[22] == 0,
            "Indirect CLI pointer-table layouts are not supported.");
        for (unsigned t=0;t<64;++t) if (valid & (U64(1) << t)) {
            row_sizes[t] = table_size(t); starts[t] = p;
            require(rows[t] <= (tables.start+tables.size-p)/row_sizes[t], "CLI table extends beyond metadata stream.");
            p += Size(rows[t])*row_sizes[t];
        }
    }
    void read_metadata(U32 rva, U32 size) {
        require(size >= 20, "Missing CLI metadata.");
        Size root = map(rva, size), end = root+size;
        require(b.u32(root) == 0x424a5342, "Invalid CLI metadata signature.");
        U32 version_length = b.u32(root+12);
        require(version_length <= size-16, "Invalid metadata version length.");
        Size p = root+16+version_length;
        require(p <= end && end-p >= 4, "Truncated metadata stream headers.");
        U32 count = b.u16(p+2); p += 4;
        std::map<std::string,Range> streams;
        for (U32 i=0;i<count;++i) {
            require(p <= end && end-p >= 9, "Truncated CLI stream header.");
            U32 off = b.u32(p), n = b.u32(p+4); p += 8;
            Size name_start=p; while (p<end && p-name_start<32 && b.u8(p)!=0) ++p;
            require(p<end && p-name_start<32, "Invalid CLI stream name.");
            std::string name(b.data.data()+name_start,b.data.data()+p);
            p = name_start + ((p-name_start+1+3)&~Size(3));
            require(p<=end && off<=size && n<=size-off, "CLI stream is outside metadata.");
            require(streams.emplace(name,Range{root+off,n}).second, "Duplicate CLI stream name.");
        }
        for (const auto& item : streams) require(item.second.start >= p, "CLI stream overlaps metadata headers.");
        for (auto i=streams.begin();i!=streams.end();++i) for (auto j=std::next(i);j!=streams.end();++j) {
            const auto& a=i->second; const auto& c=j->second;
            require(a.size==0 || c.size==0 || a.start+a.size<=c.start || c.start+c.size<=a.start,
                "Overlapping CLI metadata streams.");
        }
        require(streams.count("#Strings") && streams.count("#Blob"), "Missing CLI strings or blob heap.");
        require(streams.count("#~") + streams.count("#-") == 1, "Expected one CLI tables stream.");
        strings=streams.at("#Strings"); blobs=streams.at("#Blob");
        tables=streams.count("#~")?streams.at("#~"):streams.at("#-");
        require(strings.size && blobs.size && b.u8(strings.start)==0 && b.u8(blobs.start)==0,
            "Invalid CLI heap zero entries.");
        read_tables();
    }
    void read_pe() {
        require(b.data.size() >= 128, "File is too small to be a managed PE assembly.");
        require(b.u16(0) == 0x5a4d, "File does not contain an MZ header.");
        Size pe=b.u32(0x3c); b.check(pe,24);
        require(b.u32(pe)==0x00004550, "File does not contain a PE signature.");
        U32 section_count=b.u16(pe+6), opt_size=b.u16(pe+20);
        Size opt=pe+24; b.check(opt,opt_size);
        U32 magic=b.u16(opt); require(magic==0x10b || magic==0x20b, "Unsupported PE optional header.");
        Size dir_rel=magic==0x10b?96:112;
        require(opt_size>=dir_rel && b.u32(opt+dir_rel-4)>=15 && opt_size-dir_rel>=15*8,
            "Missing PE CLI directory.");
        Size dirs=opt+dir_rel;
        result.has_authenticode = b.u32(dirs+4*8)!=0 || b.u32(dirs+4*8+4)!=0;
        require(section_count > 0 && section_count <= 96, "Invalid PE section count.");
        Size sp=opt+opt_size; b.check(sp,Size(section_count)*40);
        for (U32 i=0;i<section_count;++i,sp+=40) {
            Section s{b.u32(sp+12),b.u32(sp+16),b.u32(sp+20)};
            b.check(s.raw_start,s.raw_size);
            require(U64(s.rva)+s.raw_size<=0x100000000ULL, "PE section RVA overflow.");
            sections.push_back(s);
        }
        U32 cor_rva=b.u32(dirs+14*8),cor_size=b.u32(dirs+14*8+4);
        require(cor_rva!=0 && cor_size>=72, "File is not a managed PE assembly.");
        Size cor=map(cor_rva,cor_size);
        require(b.u32(cor)>=72 && b.u32(cor)<=cor_size, "Invalid CLI header size.");
        cor_flags=b.u32(cor+16);
        result.has_strong_name=(cor_flags&8)!=0 || b.u32(cor+32)!=0 || b.u32(cor+36)!=0;
        native_rva=b.u32(cor+64); native_size=b.u32(cor+68);
        read_metadata(b.u32(cor+8),b.u32(cor+12));
    }
    bool generic_owner(U32 owner) const {
        for (U32 i=1;i<=rows[42];++i) if (b.index(row(42,i)+4,typeormethod())==owner) return true;
        return false;
    }
    U32 unique_type(const std::string& ns,const std::string& name) const {
        U32 found=0,count=0;
        for (U32 i=1;i<=rows[2];++i) {
            Size p=row(2,i);
            if (str(b.index(p+4,sw))==name && str(b.index(p+4+sw,sw))==ns) {found=i;++count;}
        }
        require(count==1,"Expected one "+ns+"."+name+" type; found "+std::to_string(count)+".");
        for (U32 i=1;i<=rows[41];++i) require(b.index(row(41,i),ix(2))!=found,"Expected a non-nested graphics type.");
        require(!generic_owner(found<<1),"Expected a non-generic graphics type.");
        return found;
    }
    U32 method_start(U32 type) const { return b.index(row(2,type)+4+2*sw+tdorref()+ix(4),ix(6)); }
    U32 unique_method(U32 type,const std::string& name,std::initializer_list<U8> sig) const {
        U32 first=method_start(type),last=type<rows[2]?method_start(type+1):rows[6]+1;
        require(first>=1 && last>=first && last<=rows[6]+1,"Invalid type method range.");
        U32 found=0,count=0;
        for (U32 i=first;i<last;++i) if (str(b.index(row(6,i)+8,sw))==name) {found=i;++count;}
        require(count==1,"Expected one "+name+" method; found "+std::to_string(count)+".");
        Size p=row(6,found); U32 flags=b.u16(p+6),impl=b.u16(p+4);
        require((flags&0x10)!=0 && (flags&(0x400|0x2000))==0 && (impl&3)==0 && (impl&4)==0
            && b.u32(p)!=0 && !generic_owner((found<<1)|1),name+" is not an ordinary static managed IL method.");
        require(signature(b.index(p+8+sw,bw),sig),name+" has an unrecognized method signature.");
        return found;
    }
    bool unity_getter(U32 token,const std::string& name) const {
        if ((token>>24)!=10) return false;
        Size p=row(10,token&0xffffff); Size cw=coded(3,{2,1,26,6,27});
        U32 parent=b.index(p,cw);
        if ((parent&7)!=1 || str(b.index(p+cw,sw))!=name || !signature(b.index(p+cw+sw,bw),{0,0,8})) return false;
        p=row(1,parent>>3); cw=coded(2,{0,26,35,1}); U32 scope=b.index(p,cw);
        if ((scope&3)!=2 || str(b.index(p+cw,sw))!="Screen" || str(b.index(p+cw+sw,sw))!="UnityEngine") return false;
        p=row(35,scope>>2); auto assembly=str(b.index(p+12+bw,sw));
        return assembly=="UnityEngine.CoreModule" || assembly=="UnityEngine";
    }
    Body body(U32 method) const {
        U32 rva=b.u32(row(6,method)); Size p=map(rva,1),header=0; U32 length=0;
        Body out; U8 first=b.u8(p);
        if ((first&3)==2) {header=1;length=first>>2;out.max_stack=8;}
        else {
            require((first&3)==3,"Unrecognized IL method header."); map(rva,12);
            U32 flags=b.u16(p); header=(flags>>12)*4;
            require(header>=12,"Invalid fat IL method header.");
            require((flags&8)==0,"Target method contains unexpected extra sections or exception handling.");
            out.max_stack=b.u16(p+2);length=b.u32(p+4);out.local_token=b.u32(p+8);
            require(out.local_token==0 || (out.local_token>>24)==17,"Invalid local signature token.");
        }
        require(length>0 && length<=65536,"Target method has an unexpected IL size.");
        map(rva,header+length); out.code_offset=p+header;
        U32 off=0;
        while (off<length) {
            Instruction ins; ins.offset=off;ins.op=b.u8(out.code_offset+off++);
            if (ins.op==0xfe) {
                require(off<length,"Truncated two-byte IL opcode.");ins.op=0xfe00|b.u8(out.code_offset+off++);
            }
            ins.operand_offset=off; int n=operand_size(ins.op);
            if (n<0) {
                require(length-off>=4,"Truncated IL switch.");U32 count=b.u32(out.code_offset+off);
                require(count<=(length-off-4)/4,"Invalid IL switch size.");n=static_cast<int>(4+count*4);
            }
            require(U32(n)<=length-off,"Truncated IL operand.");
            if (n==4) ins.operand=b.u32(out.code_offset+off);
            if ((ins.op>=0x2b && ins.op<=0x37) || ins.op==0xde) {
                U32 u=b.u8(out.code_offset+off); ins.branch=std::int64_t(off)+n+(u>=128?int(u)-256:int(u));
            } else if ((ins.op>=0x38 && ins.op<=0x44) || ins.op==0xdd) {
                std::int64_t delta=ins.operand; if (delta>=0x80000000LL) delta-=0x100000000LL;
                ins.branch=std::int64_t(off)+n+delta;
            }
            off+=static_cast<U32>(n);out.all.push_back(ins);if (ins.op!=0) out.code.push_back(ins);
        }
        require(!out.code.empty(),"Target method contains no executable instructions."); return out;
    }
    static void expect(const Instruction& i,unsigned op) {require(i.op==op,"Target method IL instruction shape changed.");}
    void validate_helper(const Body& x) const {
        require(x.max_stack>=2,"Ratio helper declares an insufficient evaluation stack.");
        require(x.local_token!=0 && signature(b.index(row(17,x.local_token&0xffffff),bw),{7,1,12}),
            "Ratio helper no longer has exactly one float local.");
        const auto& c=x.code;require(c.size()==15,"Ratio helper instruction shape changed.");
        const unsigned ops[]={0x04,0x05,0x5b,0x0a,0x06,0x02,0x44,0x06,0x03,0xfe03,0x16,0xfe01,0x2a,0x16,0x2a};
        for (Size i=0;i<c.size();++i) if (i!=6) expect(c[i],ops[i]);
        require(c[6].op==0x44 || c[6].op==0x37,"Ratio helper lower-bound branch changed.");
        require(x.branch_target(c[6])==13,"Ratio helper branch target changed.");
    }
    static void validate_linkage(const Body& x,U32 getter,U32 windowed) {
        const auto& c=x.code;require(c.size()>=5,"Scene-resolution guard is unexpectedly short.");
        require(c[0].op==0x28 && c[0].operand==(0x06000000|getter),"Scene-resolution guard no longer starts with the recognized aspect getter.");
        require(c[1].op==0x3a || c[1].op==0x2d,"Scene-resolution guard no longer branches on a valid aspect ratio.");
        Size skip=x.branch_target(c[1]);require(skip>=4 && skip<c.size(),"Scene-resolution guard has an unexpected success branch.");
        require(c[skip-2].op==0x28 && c[skip-2].operand==(0x06000000|windowed) && c[skip-1].op==0x2a,
            "Rejected aspect ratios no longer lead to ForceWindowed followed by return.");
        for (Size i=2;i<skip-1;++i) require(!additional_flow(c[i].op),"Aspect-rejection path now has additional control flow.");
    }
public:
    Parser(const std::vector<U8>& image,MatchResult& out):b{image},result(out){}
    void run() {
        read_pe();require(rows[32]==1,"Managed metadata is not a single assembly.");
        Size a=row(32,1);result.assembly_name=str(b.index(a+16+bw,sw));
        result.assembly_version=std::to_string(b.u16(a+4))+"."+std::to_string(b.u16(a+6))+"."+std::to_string(b.u16(a+8))+"."+std::to_string(b.u16(a+10));
        result.has_strong_name=result.has_strong_name || (b.u32(a+12)&1)!=0 || !blob(b.index(a+16,bw)).empty();
        require(!result.has_strong_name && !result.has_authenticode,
            "Signed or signature-bearing assemblies are not supported; signatures will not be removed or bypassed.");
        require(result.assembly_name=="Core","Assembly name is not Core.");
        require((cor_flags&1)!=0,"Assembly is not IL-only.");
        require(native_rva==0 && native_size==0,"Assembly has a managed native header; precompiled/native builds are not supported.");
        U32 graphics=unique_type("","SettingsPanelGraphics");
        U32 getter=unique_method(graphics,"get_IsScreenAllowableRatio",{0,0,2});
        U32 helper=unique_method(graphics,"IsAllowableRatio",{0,4,2,12,12,12,12});
        U32 windowed=unique_method(graphics,"ForceWindowed",{0,0,1});
        U32 quality=unique_type("Wotc.Mtga.Quality","QualitySettingsHelpers");
        U32 force=unique_method(quality,"ForceAllowableResolution",{0,0,1});
        Body x=body(getter);const auto& c=x.code;
        require(x.local_token==0,"Aspect getter has unexpected locals.");
        require(x.max_stack>=4,"Aspect getter declares an insufficient evaluation stack.");
        require(c.size()==8,"Aspect getter instruction shape changed.");
        expect(c[0],0x22);expect(c[1],0x22);expect(c[2],0x28);
        require(unity_getter(c[2].operand,"get_width"),"Aspect getter no longer reads UnityEngine.Screen.width.");
        expect(c[3],0x6b);expect(c[4],0x28);
        require(unity_getter(c[4].operand,"get_height"),"Aspect getter no longer reads UnityEngine.Screen.height.");
        expect(c[5],0x6b);expect(c[6],0x28);
        require(c[6].operand==(0x06000000|helper),"Aspect getter does not call the recognized ratio helper.");
        expect(c[7],0x2a);validate_helper(body(helper));validate_linkage(body(force),getter,windowed);
        float lower=as_float(c[0].operand),upper=as_float(c[1].operand);
        require(std::isfinite(lower) && lower>=1.0f && lower<=1.7f,"Unrecognized lower aspect-ratio bound (expected 1.0 through 1.7).");
        require(std::isfinite(upper) && upper>=1.75f && upper>lower,"Unrecognized upper aspect-ratio bound.");
        Size offset=x.code_offset+c[1].operand_offset;
        require(b.u32(offset)==c[1].operand,"Aspect constant file mapping failed.");
        result.upper_offset=offset;result.original_upper=upper;result.lower=lower;result.method_token=0x06000000|getter;
        result.status=upper>=result.desired_upper?MatchStatus::AlreadyCompatible:MatchStatus::Patchable;
        result.reason=result.status==MatchStatus::AlreadyCompatible
            ?"Recognized gate already accepts ratios through 2.40 or wider; no modification is needed."
            :"Recognized aspect getter, ratio calculation, and scene-resolution call linkage; four-byte upper bound can be raised to 2.40.";
    }
};
} // anonymous namespace
MatchResult inspect(const std::vector<std::uint8_t>& image) {
    MatchResult out;
    try { Parser(image,out).run(); }
    catch(const std::exception& ex) {
        out.status=MatchStatus::Unsupported;out.upper_offset=std::numeric_limits<Size>::max();out.reason=ex.what();
    }
    return out;
}
} // namespace mtga

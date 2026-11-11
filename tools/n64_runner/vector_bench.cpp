// Synthetic public-dispatch benchmark. It does not predict whole-game speed.
#include "n64/rsp_vector.hpp"
#include "n64/rsp_vector_simd.hpp"
#include "common/state.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
std::uint32_t instruction(unsigned op,unsigned dest,unsigned vs,unsigned vt,unsigned element) {
    return (0x12U<<26)|(1U<<25)|(element<<21)|(vt<<16)|(vs<<11)|(dest<<6)|op;
}
void seed(ezy::n64::RspVector& vector) {
    std::uint32_t random=0xC001CAFE;
    for(unsigned reg=0;reg<32;++reg) for(unsigned lane=0;lane<8;++lane) {
        random^=random<<13; random^=random>>17; random^=random<<5;
        vector.execute((0x12U<<26)|(4U<<21)|(reg<<11)|(lane*2<<7),random); // MTC2
    }
    vector.execute(instruction(7,31,0,8,0),random); // VMUDH initializes mixed accumulator lanes.
}
std::uint64_t checksum(ezy::n64::RspVector& vector) {
    ezy::StateWriter writer;
    vector.serialize(writer);
    std::uint64_t hash=14695981039346656037ULL;
    for(auto byte:writer.take()) hash=(hash^byte)*1099511628211ULL;
    return hash;
}
}

int main(int argc,char** argv) {
    std::uint32_t count=1'000'000;
    if(argc>1) {
        const std::string_view argument(argv[1]);
        const auto [end,error]=std::from_chars(argument.data(),argument.data()+argument.size(),count);
        if(error!=std::errc{} || end!=argument.data()+argument.size() || count<4096 || count>100'000'000) {
            std::cerr<<"Usage: ezy_rsp_vector_bench [instructions: 4096..100000000]\n";
            return 2;
        }
    }
    constexpr std::array<unsigned,3> ops{14,15,13};
    constexpr std::array names{"VMADN","VMADH","VMADM"};
    std::cerr<<"VMADN kernel: "<<(ezy::n64::detail::vmadn_avx2_available()?"AVX2":"scalar")<<'\n';
    std::cout<<"Opcode,Pattern,Repeat,Instructions,WallNsPerInstruction,Checksum\n";
    for(unsigned repeat=0;repeat<5;++repeat) for(unsigned slot=0;slot<3;++slot) {
        const unsigned index=(repeat+slot)%3;
        for(bool broadcast:{false,true}) {
            ezy::n64::RspVector vector;
            seed(vector);
            std::array<std::uint32_t,128> program{};
            for(unsigned i=0;i<program.size();++i)
                program[i]=instruction(ops[index],16+(i&15),i&7,8+((i>>3)&7),broadcast?8+(i&7):0);
            std::uint32_t scalar=0;
            for(unsigned i=0;i<4096;++i) vector.execute(program[i&127],scalar);
            const auto started=std::chrono::steady_clock::now();
            for(std::uint32_t i=0;i<count;++i) vector.execute(program[i&127],scalar);
            const auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-started).count();
            std::cout<<names[index]<<','<<(broadcast?"broadcast":"lanes")<<','<<repeat<<','<<count<<','
                     <<std::fixed<<std::setprecision(3)<<elapsed/count<<','<<checksum(vector)<<'\n';
        }
    }
}

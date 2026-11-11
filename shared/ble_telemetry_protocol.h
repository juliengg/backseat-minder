#pragma once
#include <stddef.h>
#include <stdint.h>
namespace ble_wire {
constexpr size_t HEADER_SIZE=12;
constexpr size_t MAX_PACKET=200008;
inline void put16(uint8_t *p,uint16_t n){p[0]=n;p[1]=n>>8;}
inline void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=n>>(8*i);}
inline void header(uint8_t *p,uint16_t id,uint32_t offset,uint32_t total){
    p[0]=0xb7;p[1]=0x4d;put16(p+2,id);put32(p+4,offset);put32(p+8,total);
}
}

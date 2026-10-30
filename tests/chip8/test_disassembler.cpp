#include <doctest/doctest.h>

#include "chip8/disassembler.hpp"

using ezy::chip8::disassemble;

TEST_CASE("disassemble covers each opcode family") {
    CHECK(disassemble(0x00E0) == "CLS");
    CHECK(disassemble(0x00EE) == "RET");
    CHECK(disassemble(0x1234) == "JP   234");
    CHECK(disassemble(0x2ABC) == "CALL ABC");
    CHECK(disassemble(0x6A2F) == "LD   VA, 2F");
    CHECK(disassemble(0x8124) == "ADD  V1, V2");
    CHECK(disassemble(0x812E) == "SHL  V1, V2");
    CHECK(disassemble(0xA050) == "LD   I, 050");
    CHECK(disassemble(0xD015) == "DRW  V0, V1, 5");
    CHECK(disassemble(0xE39E) == "SKP  V3");
    CHECK(disassemble(0xF233) == "LD   B, V2");
    CHECK(disassemble(0xF165) == "LD   V1, [I]");
}

TEST_CASE("disassemble marks invalid opcodes as raw data") {
    CHECK(disassemble(0x5121) == "DW   5121");
    CHECK(disassemble(0x8128) == "DW   8128");
    CHECK(disassemble(0xFFFF) == "DW   FFFF");
}

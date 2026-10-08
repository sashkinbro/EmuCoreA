package com.sbro.emucorea.core

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** A real GE-drawn PSP8888 frame: no CPU writes to VRAM or host graphics hooks. */
internal object PspGeDisplayFixture {
    /** Continuously draws RGB CC6633 through sceGe and presents it at vblank. */
    fun elf(): ByteArray {
        val entry = 0x08804000
        val loadOffset = 0x100
        val moduleOffset = 0x300
        val importsOffset = 0x340
        val displayNameOffset = 0x380
        val geNameOffset = 0x390
        val displayNidsOffset = 0x3C0
        val geNidsOffset = 0x3D0
        val displayStubsOffset = 0x3E0
        val geStubsOffset = 0x400
        val listOffset = 0x480
        val verticesOffset = 0x580
        val stringsOffset = 0x800
        val sectionsOffset = 0x900
        val bytes = ByteArray(sectionsOffset + 4 * 40)
        val data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        fun word(offset: Int, value: Int) { data.putInt(offset, value) }
        fun half(offset: Int, value: Int) { data.putShort(offset, value.toShort()) }
        fun address(offset: Int) = entry + offset - loadOffset
        fun jump(opcode: Int, target: Int) = opcode or ((target ushr 2) and 0x03FFFFFF)

        bytes[0] = 0x7F
        bytes[1] = 'E'.code.toByte(); bytes[2] = 'L'.code.toByte(); bytes[3] = 'F'.code.toByte()
        bytes[4] = 1; bytes[5] = 1; bytes[6] = 1 // ELF32, little endian, version 1.
        half(16, 2); half(18, 8) // ET_EXEC, EM_MIPS.
        word(20, 1); word(24, entry); word(28, 52); word(32, sectionsOffset)
        half(40, 52); half(42, 32); half(44, 1); half(46, 40); half(48, 4); half(50, 3)
        word(52, 1); word(56, loadOffset); word(60, entry)
        word(64, moduleOffset) // PSP module-info file offset, as in the existing guest fixture.
        word(68, 0x500); word(72, 0x1000); word(76, 7); word(80, 0x100)

        val code = mutableListOf<Int>()
        fun emit(vararg instructions: Int) { code.addAll(instructions.toList()) }
        fun loadA0(value: Int) { emit(0x3C040000 or (value ushr 16), 0x34840000 or (value and 0xFFFF)) }
        fun call(offset: Int) { emit(jump(0x0C000000, address(offset)), 0) }
        emit(0x24040000, 0x240501E0, 0x24060110) // SetMode(0, 480, 272).
        call(displayStubsOffset)
        loadA0(0x04000000)
        emit(0x24050200, 0x24060003, 0x24070001) // SetFrameBuf(VRAM, 512, PSP8888, NEXTFRAME).
        call(displayStubsOffset + 8)
        val loopAddress = entry + code.size * 4
        loadA0(address(listOffset))
        emit(0x24050000, 0x2406FFFF, 0x24070000) // EnQueue(list, no stall, no callback, no options).
        call(geStubsOffset)
        emit(0x00402021, 0x24050000) // ListSync(returned list ID, WAIT).
        call(geStubsOffset + 8)
        call(displayStubsOffset + 16) // WaitVblankStart yields the guest thread.
        emit(jump(0x08000000, loopAddress), 0)
        check(loadOffset + code.size * 4 < moduleOffset)
        code.forEachIndexed { index, instruction -> word(loadOffset + index * 4, instruction) }

        half(moduleOffset, 0); half(moduleOffset + 2, 0x0100)
        "NativeGeDisplay".toByteArray(Charsets.US_ASCII).copyInto(bytes, moduleOffset + 4)
        word(moduleOffset + 44, address(importsOffset))
        word(moduleOffset + 48, address(importsOffset + 40))
        fun imports(offset: Int, name: String, nameOffset: Int, nidsOffset: Int, stubsOffset: Int, nids: IntArray) {
            name.toByteArray(Charsets.US_ASCII).copyInto(bytes, nameOffset)
            word(offset, address(nameOffset))
            half(offset + 4, 0x0100); half(offset + 6, 0x4001)
            bytes[offset + 8] = 5 // Five-word PspLibStubEntry.
            half(offset + 10, nids.size)
            word(offset + 12, address(nidsOffset)); word(offset + 16, address(stubsOffset))
            nids.forEachIndexed { index, nid ->
                word(nidsOffset + index * 4, nid)
                word(stubsOffset + index * 8, 0x03E00008) // jr ra; patched by the PSP loader.
                word(stubsOffset + index * 8 + 4, 0)
            }
        }
        // NIDs verified against Core/HLE/sceDisplay.cpp and Core/HLE/sceGe.cpp.
        imports(importsOffset, "sceDisplay", displayNameOffset, displayNidsOffset, displayStubsOffset,
            intArrayOf(0x0E20F177, 0x289D82FE, 0x984C27E7.toInt()))
        imports(importsOffset + 20, "sceGe_user", geNameOffset, geNidsOffset, geStubsOffset,
            intArrayOf(0xAB49E76A.toInt(), 0x03444EB4))

        fun ge(command: Int, argument: Int = 0) = (command shl 24) or (argument and 0x00FFFFFF)
        val bottomRight = (271 shl 10) or 479
        val vertexAddress = address(verticesOffset)
        // Commands and vertex flags verified against GPU/ge_constants.h.
        val list = intArrayOf(
            ge(0x10, (vertexAddress ushr 8) and 0x000F0000), // BASE high address bits.
            ge(0x13), ge(0x01, vertexAddress), // OFFSETADDR=0, VADDR low 24 bits.
            ge(0x12, (1 shl 23) or (3 shl 7) or (7 shl 2)), // THROUGH, XYZ float, RGBA8888.
            ge(0x9C), ge(0x9D, 0x040200), ge(0xD2, 3), // VRAM base 0, stride 512, PSP8888.
            ge(0x15), ge(0x16, bottomRight), // Drawing region 0,0 through 479,271.
            ge(0xD4), ge(0xD5, bottomRight), // Matching scissor.
            ge(0xD6), ge(0xD7, 0xFFFF), // Full depth range, matching PPGeBegin.
            ge(0x4C), ge(0x4D), // No screen-space offsets.
            ge(0x17), ge(0x1D), ge(0x1E), ge(0x1F), ge(0x20), // No lights/cull/texture/fog/dither.
            ge(0x21), ge(0x22), ge(0x23), ge(0x24), ge(0x25), ge(0x27), ge(0x28),
            ge(0xD3), ge(0xE7, 1), ge(0xE8), ge(0xE9), // Normal draw, no depth writes, all color channels.
            ge(0x50, 1), // Gouraud: both vertices have the same color.
            ge(0x04, (6 shl 16) or 2), // GE_PRIM_RECTANGLES, two vertices.
            ge(0x0F), ge(0x0C) // FINISH, END.
        )
        check(listOffset + list.size * 4 <= verticesOffset)
        list.forEachIndexed { index, command -> word(listOffset + index * 4, command) }
        for ((index, position) in listOf(0f to 0f, 480f to 272f).withIndex()) {
            val offset = verticesOffset + index * 16
            word(offset, 0xFF3366CC.toInt()) // PSP ABGR8888 storage -> RGB CC6633.
            data.putFloat(offset + 4, position.first)
            data.putFloat(offset + 8, position.second)
            data.putFloat(offset + 12, 0f)
        }

        val names = "\u0000.text\u0000.rodata.sceModuleInfo\u0000.shstrtab\u0000"
        names.toByteArray(Charsets.US_ASCII).copyInto(bytes, stringsOffset)
        fun section(index: Int, name: String, flags: Int, sectionAddress: Int, offset: Int, size: Int) {
            val header = sectionsOffset + index * 40
            word(header, names.indexOf(name)); word(header + 4, 1); word(header + 8, flags)
            word(header + 12, sectionAddress); word(header + 16, offset); word(header + 20, size)
            word(header + 32, 4)
        }
        section(1, ".text", 6, entry, loadOffset, code.size * 4)
        section(2, ".rodata.sceModuleInfo", 2, address(moduleOffset), moduleOffset, 52)
        section(3, ".shstrtab", 0, 0, stringsOffset, names.length)
        word(sectionsOffset + 3 * 40 + 4, 3) // SHT_STRTAB.
        return bytes
    }
}

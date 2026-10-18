OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    /* merge .bss into .data so the flat binary carries zero-filled space;
     * the loader maps only pages covered by the binary image. */
    .data : {
        *(.data*)
        *(.bss*)
        *(COMMON)
    }
}

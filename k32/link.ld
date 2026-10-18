OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text.entry)
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    __bss_end = .;

    /* The boot stack sits at 0x9F000 and grows down.  Keep .bss well below
     * it (and below VGA memory at 0xA0000), otherwise large static buffers
     * silently overwrite the stack. */
    ASSERT(__bss_end <= 0x9E000, "kernel .bss overlaps the 0x9F000 boot stack")
}

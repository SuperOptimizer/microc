ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text : { *(.head) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(4096);
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(4096);
    _end = .;
}

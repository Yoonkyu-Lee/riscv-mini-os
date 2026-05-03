OUTPUT_ARCH("riscv")

PHDRS {
  text PT_LOAD;
  rodata PT_LOAD;
  data PT_LOAD;
}

SECTIONS {
  . = 0x80000000;

  PROVIDE(_kimg_start = .);

  .text : {
    PROVIDE(_kimg_text_start = .);
    *(.text.start) /* entry */
    *(.text .text.*)
    . = ALIGN(16);
    PROVIDE(_kimg_text_end = .);
  } :text

  . = ALIGN(4096);

  .rodata : {
    PROVIDE(_kimg_rodata_start = .);
    *(.srodata .srodata.*)
    . = ALIGN(16);
    *(.rodata .rodata.*)
    PROVIDE(_kimg_rodata_end = .);
  } :rodata

  . = ALIGN(4096);

  .data : {
    PROVIDE(_kimg_data_start = .);
    PROVIDE(_kimg_blob_start = .);
    *(*.data.blob)
    *(*.rodata.blob)
    PROVIDE(_kimg_blob_end = .);
    . = ALIGN(16);
    *(.sdata .sdata.*)
    . = ALIGN(16);
    *(.data .data.*)
    . = ALIGN(16);
  } :data

  .bss : {
    *(.data.pagetable)
    *(.sbss .sbss.*)
    . = ALIGN(16);
    *(.bss .bss.*)
    . = ALIGN(16);
    PROVIDE(_kimg_data_end = .);
  } :data

  PROVIDE(_kimg_end = .);
}
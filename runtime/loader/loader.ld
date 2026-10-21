/* SPDX-License-Identifier: GPL-3.0-only */
ENTRY(pocket_entry)
PHDRS {
  headers PT_PHDR PHDRS;
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  ro PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
  probe PT_LOAD FLAGS(5);
  stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
  . = 0x5000000000 + SIZEOF_HEADERS;
  .text : ALIGN(16) { *(.text .text.*) } :text
  . = ALIGN(0x4000);
  .rodata : { *(.rodata .rodata.*) } :ro
  . = ALIGN(0x4000);
  .data : { *(.pocket_config) *(.data .data.*) } :data
  .bss (NOLOAD) : { *(.bss .bss.* COMMON) } :data
  ASSERT(. < 0x5000200000, "loader exceeds syscall bypass window")
  . = 0x5000300000;
  .pocket_probe : { *(.pocket_probe) } :probe
  /DISCARD/ : { *(.comment .note.* .eh_frame .eh_frame_hdr) }
}

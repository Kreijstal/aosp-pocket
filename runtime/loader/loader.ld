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
  /* Older Android interpreter loaders do not allocate BSS pages in this
     intermediate segment when a later PT_LOAD has a higher file end. Keep
     zero-initialized runtime state file-backed as well, so Binder state that
     grows beyond the first 4 KiB page is actually mapped on Samsung 4.19. */
  .data : {
    *(.pocket_config) *(.data .data.*) *(.bss .bss.* COMMON)
    . = ALIGN(0x4000);
  } :data
  ASSERT(. < 0x5000200000, "loader exceeds syscall bypass window")
  . = 0x5000300000;
  /* Older kernels clear the final interpreter page during exec. Keep this
     RX segment page-complete so that zeroing never writes into executable code. */
  .pocket_probe : { *(.pocket_probe) . = ALIGN(0x4000); } :probe
  /DISCARD/ : { *(.comment .note.* .eh_frame .eh_frame_hdr) }
}

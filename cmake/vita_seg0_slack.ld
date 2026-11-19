/* Ends the read-only segment one byte past a page boundary so vita-elf-create always has a page of slack for its SCE data. */
SECTIONS
{
  .vita_seg0_slack : { *(.vita_seg0_slack) . = ALIGN(0x1000) + 1; }
}
INSERT AFTER .eh_frame;

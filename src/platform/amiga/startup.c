/* libnix's crt0 (ncrt0.o, selected via -mcrt=nix) requires this control
 * variable to be defined by the application. Non-zero means "don't require
 * a CLI command line to be parsed/present" -- appropriate for an emulator
 * frontend that takes its ROM path as its one real argument either way. */
int __nocommandline = 1;

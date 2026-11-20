# BOTWRecompAndroid 0.3.4-beta (beta 31)

A small compatibility hotfix for 6 GB devices. Version 0.3.3 automatically enabled the memory-saving profile on devices reporting less than 6 GiB of RAM, including phones sold with 6 GB. Some users reported errors that did not occur before. The automatic profile is now limited to devices with up to 4 GB; 6 GB devices return to the normal texture, buffer, and cache settings. The manual `LARECOMP_LOW_RAM` override remains available, and Mali's separate pipeline limit is unchanged.

The fix has been built but still needs confirmation on affected 6 GB devices. If the problem persists, please open an issue with your device model, GPU, driver, and log.

Install over 0.2.x or 0.3.x without rebuilding the game code. The project is still in beta, and no game files are included.

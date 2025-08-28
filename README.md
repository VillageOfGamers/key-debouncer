# Key Debouncer

## About
This is a quick and simple per-key keyboard input de-bouncer specifically designed to debounce any standard English-US keyboard, with or without the number pad and/or media keys. It can be repurposed for other layouts as needed by adjusting which keys are tied to which codes (the list is currently hardcoded for `en-US` layout, non-Dvorak).

## Contributing
If you would like to contribute to this project, please fork this repo, make your changes, and submit a pull request! All are welcome to do so, though it will still be up to me on which ones get added/implemented.

## How to Use
1. Make sure you can build basic C programs, have your kernel headers, and that the `uinput` module is installed AND loaded (99% of the time it will be unless you removed it yourself).

2. Run `./list-keyboards.sh` to list off your current keyboards. It'll list their friendly name and symlink under `/dev/input/by-id/`.

3. Provide the symlink name to the script `debounce.sh`, along with a preferred timeout in milliseconds (33ms is a good default). The sample script points to a Corsair K70 Core RGB keyboard (mine has lots of very bouncy keys and is why this project was made).

4. Simply run `sudo make install` and it'll build the executable, then proceed to install it and the wrapper script to `/usr/local/bin`, and the SystemD unit file to `/etc/systemd/system` (default Ubuntu locations for non-APT-managed programs).

5. If the keyboard still acts bouncy (double inputs still occur), try increasing the timeout slightly. I recommend increasing the timeout in 5ms steps. Repeat until you can comfortably say that it won't bounce under normal usage. I use 15ms myself and it's the default for the project as well. If you find that it works and doesn't hinder your own usage, you probably should keep it.

## Reporting Issues
If you run into problems with this project, have a terminal open in the project folder and follow these steps:

1. Rebuild the program using `make MODE=dev`. It'll generate the binary as `debounce` in the `dist/` folder along with the `debounce.pdb` file for actually debugging it.

2. Run `./debounce <kbname> <timeout>` replacing `<symlink>` with your keyboard's real symlink under `/dev/input/by-id` and `<timeout>` with a number in milliseconds (1 is 1ms, 10 is 10ms). Preferably, attach a debugger to the process (like `gdb` or simiilar) to see what it's doing internally.

    * If the program works with `make MODE=dev`, but not with just `make`, it's a compiler optimization problem. In this case:

        * Save the FULL output from the command (using both `make` and `make MODE=dev` separately) along with which version of the program you're using.

        * Also get the version of the compiler you're using (for GCC, use `gcc -v`). I would need the full output so I can report back to the proper compiler teams; it's also vital I know which compiler I'm looking at version strings from!

    * If it's not working using `make MODE=dev` either, then:

        * Save the FULL output from the command from step 2, along with which version you're using, the keyboard you're trying to use and its device path in `/dev/input/` from the `list-keyboards.sh` script provided.

        * If your keyboard is NOT appearing using that script, or you don't know which one it is due to the name not populating, the symlink name should reveal what keyboard it's pointing at.

        * If you have more than one keyboard connected, and they're of the same brand and model, it can be difficult to see which one is which; unplug the one you are NOT using and re-run `list-keyboards.sh` to see only the desired entry, then re-run the program with the right symlink.

        * If it works as intended with the correct symlink, run `make clean` then `sudo make install`, and proceed to use the program; no issue required. If it's still acting bouncy, try adjusting timeout values.

3. Now that you've determined what you need, create an issue on GitHub, and provide the info requested based on your circumstances. Please also tell me what Linux distribution, desktop environment (like GNOME, KDE, Xfce, etc.), and render backend (X11 or Wayland) you're using. For reference, I made this and use it daily on Linux Mint 22.1 Cinnamon Edition, under X11.

## Other OS Support
I do not plan to support Windows or MacOS with this at all; Windows generally has working debounce utilities, and MacOS is not an OS I have reasonable access to at this time. Please do not ask for these; it's just not going to happen. This tool is, by design, Linux-only. Not even Android or iOS will be supported. Also, I apologize in advance if BSD or other Unix-like OSes have issues; I have no way to test them.
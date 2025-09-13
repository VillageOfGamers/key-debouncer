# Key Debouncer

## About
This is a quick and simple per-key keyboard input debouncer specifically designed to debounce any standard English-US keyboard, with or without the number pad and/or media keys. It can be repurposed for other layouts as needed by adjusting which keys are tied to which codes (the list is currently hardcoded for `en-US` layout, non-Dvorak).

## Contributing
If you would like to contribute to this project, please fork this repo, make your changes, and submit a pull request! All are welcome to do so, though it will still be up to me on which ones get added/implemented.

## How to Use
1. Make sure you can build basic C programs, have your kernel headers, and that the `uinput` module is installed AND loaded (99% of the time it will be unless you removed it yourself).

2. Build the program using `make` and run `sudo ./bin/debouncectl show` to get a filtered list of physical keyboard device nodes. Find your keyboard and remember which one it is.

3. Run `sudo make install` to install the software to be installed properly; expect some minor compiler complaints. The program does still function despite them.

4. Logout then log back in; you'll now be part of the `input` group on your system. This is required to be able to see nodes inside `/dev/input` without `sudo`.

5. Once that's done, run `sudo systemctl daemon-reload` followed by `sudo systemctl enable --now debounced` to start the actual background process that handles the keyboard filtering.

6. Last but not least, run `debouncectl --help`, read it for instructions on how to use it properly, and test your keyboard afterwards! If it's still bouncing, re-run `debouncectl` with a higher timeout value, and try again.

## Reporting Issues
If you run into problems with this software, please create an issue on the repository. I'll check on it often to make sure it's working as intended, and if issues arise, I will help diagnose problems as they come up.

## Other OS Support
I do not plan to support Windows or MacOS with this at all; Windows generally has good, working debounce utilities, and MacOS is not an OS I have reasonable access to at this time. Please do not ask for these; it's just not going to happen. This tool is, by design, Linux-only. Not even Android or iOS will be supported due to system limitations (root access required to hijack input HALs). Also, I apologize in advance if BSD or other Unix-like OSes have issues; I have no way to test them as my daily driver is an Ubuntu-based Linux distribution.

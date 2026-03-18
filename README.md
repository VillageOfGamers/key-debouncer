# Key Debouncer

## About
This is a quick and simple per-key keyboard input debouncer specifically designed to debounce **any standard keyboard**, regardless of keyboard layout or input language; `libevdev` provides human-readable keycode names for logging, though debounce logic operates on raw kernel keycodes which are layout-agnostic by design. It also includes FlashTap, a directional input feature designed for gaming that eliminates the mechanical deadband between releasing one directional key and pressing the other.

## Installing from a Package Repository

### Debian, Ubuntu, and derivatives
```bash
curl -fsSL https://villageofgamers.github.io/key-debouncer-dist/debounced-archive-keyring.asc | sudo gpg --dearmor -o /etc/apt/keyrings/debounced.gpg
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/debounced.gpg] https://villageofgamers.github.io/key-debouncer-dist/apt stable main" | sudo tee /etc/apt/sources.list.d/debounced.list
sudo apt-get update
sudo apt-get install debounced
```

### Fedora, RHEL, and derivatives
```bash
sudo rpm --import https://villageofgamers.github.io/key-debouncer-dist/debounced-archive-keyring.asc
sudo tee /etc/yum.repos.d/debounced.repo << EOF
[debounced]
name=Key Debouncer
baseurl=https://villageofgamers.github.io/key-debouncer-dist/rpm/x86_64
enabled=1
gpgcheck=0
repo_gpgcheck=1
gpgkey=https://villageofgamers.github.io/key-debouncer-dist/debounced-archive-keyring.asc
EOF
sudo dnf install debounced
```

### Arch Linux (AUR)
```bash
yay -S debounced
# or with paru:
paru -S debounced
```

## Building from Source

1. Clone the repository using `git clone https://github.com/VillageOfGamers/key-debouncer.git` and then `cd` into the directory that gets created.

2. Install the following items: GCC, basic build tools, and libevdev headers. On Debian and Ubuntu, these packages: `gcc libevdev-dev build-essential` are the ones you need. Find your equivalents in your package manager if you're on another distro.

3. Build the program using `make` and run `sudo ./bin/debouncectl show` to get a filtered list of potential keyboard device nodes. Find your keyboard and remember which one it is.

4. Test out the program by running `nohup ./bin/debounced &` in order to force the daemon to the background, then run `debouncectl start <device>` to make the daemon latch to your keyboard.

5. If you are satisfied with the end result, run `sudo make install` in order to install the systemd unit file inside `/etc/systemd/system/` and the binaries themselves into `/usr/local/bin/` (these are default paths for non-packaged applications on Debian-based systems).

6. Log out, and then immediately log back in; you will now be part of the `input` group on your system. This is required to be able to see nodes inside `/dev/input` without `sudo` (this is how the `show` subcommand works).

7. Once that's done, run `sudo systemctl daemon-reload` followed by `sudo systemctl enable --now debounced` to start the daemon; without it alive, the control program does nothing whatsoever.

8. Last but not least, run `debouncectl --help`, read it for instructions on how to use it properly, and test your keyboard afterwards! If it's still bouncing, run `debouncectl stop` followed by `debouncectl start <device> [timeout]`. The timeout can be anywhere from 1 to 250, and is in milliseconds.

## Contributing
If you would like to contribute to this project, please fork this repository, make your changes, and submit a pull request! All are welcome to do so, however acceptance of pull requests is at the discretion of the project maintainer (currently me, @Giantvince1).

## Reporting Issues
If you run into problems with this software, please create an issue on the repository. I'll check on it often to make sure it's working as intended, and if issues arise, I will help diagnose problems as they come up.

## OS Support
This program supports Linux in almost any flavor — Arch, Debian, Ubuntu, Gentoo, Fedora, RHEL, Oracle Linux, and beyond. The only requirements are a kernel built with `uinput` support (enabled by default on virtually all mainstream distributions) and a glibc-based userspace. musl-based distributions such as Alpine Linux are not currently supported.

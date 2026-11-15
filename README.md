# VitaSSH – SSH client and server for PS Vita

VitaSSH is a proof-of-concept SSH client designed specifically for the PlayStation Vita, leveraging **libssh2** for secure shell connections and **VitaSDK** for native Vita hardware integration. This experimental client provides basic SSH terminal functionality on the Vita's unique platform.

> Prior art: this began as a fork of [VitaSSH](https://github.com/rompelhd/VitaSSH) by
> rompelhd, and its packaging and installation follow the conventions of
> [VitaShell](https://github.com/TheOfficialFloW/VitaShell). Both are the reason any of
> this was possible; the code in this repo has since diverged considerably.

## Screenshot
<img width="960" height="544" alt="image" src="https://github.com/user-attachments/assets/6dd46da1-1ae0-4790-9b9a-85d6aa143d9d" />


## 🛠 Technical Foundation

**Built with:**
- **Libssh2**: Comprehensive SSH2 protocol implementation.
- **Vita2d**: Vita2d library for creating 2D graphics.
- **VitaSDK**: Official PlayStation Vita homebrew SDK.
- **IME Dialog System**: Vita's on-screen keyboard interface.

## ⚡ Features and Limitations

### ✅ Working Features
- Secure SSH2 connections with password authentication.
- Arbitrary command execution on remote systems.
- Special character filtering for Vita display compatibility.
- Network state monitoring with visual feedback.
- Memory-safe resource cleanup.

### ⚠️ Current Limitations
- No scrollback: Screen clears after each command.
- No command history: Manual retyping required.
- No touch input: Physical buttons only (TRIANGLE/SQUARE).
- Limited character set: ASCII-only display output.
- No interactive sessions: Single commands only.
- Experimental status: Not for production use.

## 🔄 Technical Challenges Solved
- **Vita IME Integration**: Bridging UTF-16 keyboard to ASCII SSH.
- **Graphics Context**: One vita2d context for everything, including the text
  console (this SDK has no `sceDebugScreenInit`, so there is no debug screen to
  switch to).
- **Memory Management**: CDRAM/User RAM allocation strategies.
- **Network Reliability**: Vita-specific NetCtl state handling.
- **Output Sanitization**: ANSI/Unicode filtering for text display.

## 🚀 Build & Deployment

```bash
task setup       # checks vitasdk, builds OpenSSL + libssh2 + libssh into the SDK (skips when installed)
task build       # configures and builds both apps' SELF + VPK; skips when nothing changed
task clean       # remove build/, task's fingerprints, and the staged portlib sources
```

`task` (https://taskfile.dev, e.g. `brew install task` or `mise use -g task`) is the
entry point; CMake does the rest. `VITASDK` resolves from `task build VITASDK=<path>`,
then `.env`, then `~/.local/opt/vitasdk` — run `cp .env.example .env` and set it there for
a machine-local default (changing it needs a fresh `build/`, since `CMakeCache.txt` pins
absolute toolchain paths). The first `task setup` compiles the
`libs/*/VITABUILD` port recipes into `$VITASDK` — the step the README used to omit,
which is why a fresh clone failed on a missing `libssh2.h`. Force one package
after editing its recipe with `task --force setup:openssl` / `setup:libssh2` /
`setup:libssh`.

## 📦 Generates

Two apps, two VPKs, one `task build`:

- `ssh_client_vita.self` / `ssh_client_vita.vpk` — the SSH client (`ROM000069`),
  libssh2 over a vita2d terminal
- `ssh_server_vita.self` / `ssh_server_vita.vpk` — the SSH server (`ROM000070`),
  libssh with the builtin shell and SFTP; log in to the Vita from a devbox

Each app ships its own tile: the client's is `sce_sys/`, the server's is `sce_sys_sshd/`,
and both are rendered from the SVG sources in `assets/` (the render command is in each
file's header).

---

## 🎯 Use Cases

- Remote server administration from the PS Vita  
- Quick file system inspection using `ls`, `df`, etc.  
- Network troubleshooting with `ping`, `netstat`  
- Educational demonstration of Vita homebrew capabilities  
- SSH protocol testing in constrained environments

---

## 📋 Prerequisites

- PlayStation Vita with homebrew enabled  
- VitaSDK development environment  
- `libssh2` library compiled for Vita  
- OpenSSL libraries for Vita

---

## 🔧 Installation

1. Copy the generated `.vpk` file to your Vita.  
2. Install it using VitaShell or the Package Manager.  
3. Make sure you have network connectivity (WiFi required).  
4. Launch VitaSSH and configure your connection details.

---

## 🎮 Controls

- `TRIANGLE`: Open on-screen keyboard for commands  
- `SQUARE`: Exit the application  
- `D-Pad/Stick`: Navigate through IME dialogs  
- `X Button`: Confirm/enter in dialogs

---

## ⚠️ Important Notes

- This is **experimental software** — use at your own risk.  
- Some SSH servers may require specific configuration.  
- Complex commands with extensive output may cause display issues.  
- Password authentication only (no key-based authentication yet).

---

## 🐛 Known Issues

- Limited output buffer for long command responses  
- No support for interactive programs (`vim`, `top`, etc.)  
- ~~Character encoding limitations for non-ASCII text~~ ✅ 
- Occasional timeouts on unstable network connections

---

## 🤝 Contributing

This project is open to contributions! Areas for improvement:

- Scrollback functionality  
- Command history  
- Touchscreen integration  
- SSH key authentication  

---

> **Experimental • Proof of Concept • Vita Homebrew**  
> *Powered by libssh2 1.11.0+ and VitaSDK toolchain for native Vita SSH capabilities.*

**Disclaimer:** This software is provided *as-is* for educational and experimental purposes. Use responsibly and in compliance with applicable laws and terms of service.

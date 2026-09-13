<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Edge - Xbox 360 Emulator</h1>

Xenia Edge is yet another experimental fork of the Xenia emulator, originally based on [Xenia Canary](https://github.com/xenia-canary/xenia-canary). The focus is
on faster iteration, higher default game compatibility, usability and platform support.

## Status

Build (Windows / Linux / macOS): [![CI](https://github.com/has207/xenia-edge/actions/workflows/CI.yml/badge.svg?branch=edge)](https://github.com/has207/xenia-edge/actions/workflows/CI.yml) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/has207/xenia-edge/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

Releases
--------
[Latest](https://github.com/has207/xenia-edge/releases/latest) ◦ [All](https://github.com/has207/xenia-edge/releases)

FAQ
---

- Q: How to tell what options a particular game might need?<br>
  A: Many games that are not running well by default require one or two simple config changes that make them (near) perfect. To find out, check the Compatibility section in game details pane. If there is an existing compatibility page there will be a link to Master, Canary, or both. Prefer information on the Canary page, you will often find the answer there.

- Q: Why are translations into language X so bad?<br>
  A: They're AI-generated, if you speak the language and want to help edit the relevant .po file in the assets directory and submit it. If you don't know how to use git then just open a bug and attach the fixed .po file. Best way to edit those is with a program called Poedit. Or just open a bug and tell me what translation is bad and how to improve it.

- Q: Should I use the Windows build with wine/proton on Linux?<br>
  A: While the answer has been "yes" for years it is no longer the case. The native build for Linux is quite competitive with Windows now and is recommened over wine/proton.

- Q: Why is macOS build not as capable as Windows / Linux?<br>
  A: macOS is the newest port, with a completely different CPU and GPU backends that have not had nearly the amount of testing of the other platforms. Many games will fail on macOS that run on Windows and Linux. This is working as intended for now.

- Q: Can you add ARM64 Linux or Windows builds?<br>
  A: Yes, and I choose not to, there is no need to send me PRs or requests for these builds. They will be made available when I'm ready to support them, in the meantime you can build from source if you really want to run them.
  
- Q: Will you accept donations of money or games/hardware for testing?<br>
  A: No, this project is strictly a hobby and I would like to keep it that way. I pay for all games/hardware I test out of my own pocket and do not want to receive any compensation for my work on Xenia.


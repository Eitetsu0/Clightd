# Clightd [![builds.sr.ht status](https://builds.sr.ht/~fededp/clightd.svg)](https://builds.sr.ht/~fededp/clightd?)

[![Packaging status](https://repology.org/badge/vertical-allrepos/clightd.svg)](https://repology.org/project/clightd/versions)

Clightd is a bus interface that lets you easily set/get screen brightness, gamma temperature and display dpms state.  
Moreover, it enables getting ambient brightness through webcam frames capture or ALS devices.  

It works on both X, Wayland and tty.  
On wayland specific protocols need to be implemented by your compositor; have a look at {Gamma, Dpms, Screen} wiki pages.  

Clightd is used as a backend by [Clight](https://github.com/FedeDP/Clight) but it can be used without it too.  

**For a guide on how to build, features and lots of other infos, refer to [Clightd Wiki Pages](https://github.com/FedeDP/Clightd/wiki).**  
**Note that Wiki pages will always refer to master branch.**  
*For any other info, please feel free to open an issue.*  

## Arch AUR packages
Clightd is available on AUR as both stable or devel package: https://aur.archlinux.org/packages/?K=clightd .  
Note that devel package may break in some weird ways during development. Use it at your own risk.

## License
This software is distributed with GPL license, see [COPYING](https://github.com/FedeDP/Clightd/blob/master/COPYING) file for more informations.

### A port in progress of jamin to Haiku OS

### Dependencies for building 64bit
```
pkgman install fftw_devel gtk3_devel libxml2_devel gettext make automake autoconf libtool intltool pkgconfig glib2_devel
``
#### 32bit
```
pkgman fftw_devel gtk3_x86_devel libxml2_devel gettext make automake autoconf libtool_x86 intltool pkgconfig glib2_x86_devel
```
### Dependencies for running 32bit
```
pkgman install fftw gtk3 libxml2 glib2
```

#### 32bit
```
pkgman fftw gtk3_x86 libxml2_x86 glib2_x86
```

### Build / Install libspa 64 Bit
```
git clone https://github.com/swh/ladspa.git
cd ladspa
# 64bit
pkgman install libtool list_moreutils list_moreutils_xs ladspa_sdk_devel
./autogen.sh
./configure --prefix=/boot/home/config/non-packaged
curl -L https://cpanmin.us | perl - --self-upgrade
cpanm List::MoreUtils
make
make install
```

### Build / Install libspa 32 Bit
```
pkgman install libtool_x86 list_moreutils_xs_x86 ladspa_sdk_x86_devel
./autogen.sh
./configure --prefix=/boot/home/config/non-packaged
curl-x86 -L https://cpanmin.us | perl - --self-upgrade
cpanm List::MoreUtils
# You will probably get an error: MoreUtils not installed... run this next
setarch x86 cpanm --force Test::LeakTrace 

# You will need to export this but noticed your file location name probably will be different. 
# The import thing is *List-MoreUtils-0.430/lib/

export PERL5LIB=/boot/home/.cpanm/work/1777165559.2771/List-MoreUtils-0.430/lib/

setarch x86 make CFLAGS="-D'LADSPA_Data=float'"
# if the above fails try: make clean; setarch x86 make

# Last thing to do is install
# JAMin will copy the Ladspa files so you can delete them once JAMin is installed.
# `make uninstall` after JAMin is installed might work. If not, find Ladspa in /boot/home/config/non-packaged/lib/ladspa and delete.

make install
```


### Build / Test jamin:
```
make -f haiku.makefile release
open jamin-0.98.9-1-x86_64.hpkg

```


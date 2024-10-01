# VM Course HW 1

## Usage

After `git clone`, please initialize all submodules:
```
git submodule update --init --recursive
```

```sh
mkdir build
cd build
cmake ..
make
./measure
```

## Output for my machine

```sh
$ ./measure
Size = 32768
Assoc = 8
BlockSize = 64
```

## Real answer for my machine

AMD Ryzen 7 5800U with Radeon Graphics, 8 cores.

L1 Data Cache:
- Size: 32 KiB for each of 8 cores.
- Cache line size: 64 bytes.
- Ways of associativity: 8.

```
[~] cat /sys/devices/system/cpu/cpu0/cache/index0/level
1
[~] cat /sys/devices/system/cpu/cpu0/cache/index0/type
Data
[~] cat /sys/devices/system/cpu/cpu0/cache/index0/size
32K
[~] cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
64
[~] cat /sys/devices/system/cpu/cpu0/cache/index0/ways_of_associativity
8
```

# MidiAppBox → Kybotos

> **このリポジトリは移転しました。開発は [kybotos/kybotos](https://github.com/kybotos/kybotos) で続けています**(名前を Kybotos に変えました)。
> ここは過去の記事から参照されているため、Phase 18 の時点のまま残し、アーカイブしています。
>
> **This repository has moved.** Development continues at [kybotos/kybotos](https://github.com/kybotos/kybotos) under the new name *Kybotos*.
> This repository is kept as of Phase 18 and archived.


## Demo

[![Demo video](https://img.youtube.com/vi/UdiFrxvP_qg/0.jpg)](https://youtu.be/UdiFrxvP_qg)

## Prepare using docker container

```
DEV=/dev/ttyACM0;docker run --rm -it -v ${PWD}:/workspaces/MidiAppBox -w /workspaces/MidiAppBox --device=${DEV} --group-add $(stat -c '%g' ${DEV}) ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:5.5.5
```

## Build

```
cd src
idf.py build
```

## Write flash

```
cd src
idf.py flash
```


## Prepare using docker container

```
DEV=/dev/ttyACM0;docker run --rm -it -v ${PWD}:/workspaces/MidiAppBox -w /workspaces/MidiAppBox --device=${DEV} --group-add $(stat -c '%g' ${DEV}) ghcr.io/wurly200a/builder-esp32/esp-idf-v5.5:latest
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

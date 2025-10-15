

## build

```
docker run --rm -it -v ${PWD}:/workspaces/MidiAppBox -w /workspaces/MidiAppBox ghcr.io/wurly200a/builder-esp32/esp-idf-v5.3:latest
cd src
idf.py build
```

```
DEV=/dev/ttyACM0;docker run --rm -it -v ${PWD}:/workspaces/MidiAppBox -w /workspaces/MidiAppBox --device=${DEV} --group-add $(stat -c '%g' ${DEV}) ghcr.io/wurly200a/builder-esp32/esp-idf-v5.3:latest
```

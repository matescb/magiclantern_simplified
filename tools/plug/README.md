# SONOFF iPlug S60 LAN control

CLI for the SONOFF S60 smart plug at `192.168.1.52` using the encrypted
eWeLink LAN protocol. No dependencies beyond system `requests` + `cryptography`.

## One-time setup

```sh
python3 plug.py setup
```

Prompts for your eWeLink email + password, fetches the plug's `devicekey` from
the cloud, and saves it to `~/.config/sonoff_plug.json` (chmod 600).
Credentials are used once for the login and are **not** stored — only the
devicekey is. After setup everything runs purely on the LAN (works offline).

Already have the key? `python3 plug.py setup --devicekey <uuid>`

## Usage

```sh
python3 plug.py on
python3 plug.py off
python3 plug.py toggle
python3 plug.py status          # human-readable
python3 plug.py status --json   # full state as JSON
```

## Tests

```sh
python3 -m pytest test_plug.py
```

from pymodbus.client import ModbusSerialClient

PORT = "/dev/cu.usbserial-1130"  # change to your port
BAUDRATES = [9600, 4800, 19200, 2400, 38400]
PARITIES = ["N", "E", "O"]
STOPBITS = [1, 2]
UNITS = range(1, 9)
REGS = range(0, 8)
FUNCS = [3, 4]  # 03 holding, 04 input

def read_regs(client, kind, address, count, unit_id):
    func = client.read_holding_registers if kind == 3 else client.read_input_registers
    errors = []

    for keyword in ("device_id", "unit", "slave"):
        try:
            return func(address=address, count=count, **{keyword: unit_id})
        except TypeError as exc:
            errors.append(f"{keyword}: {exc}")

    try:
        return func(address, count, unit_id)
    except TypeError as exc:
        errors.append(f"positional: {exc}")
        raise TypeError("; ".join(errors))

def try_one(baudrate, parity, stopbits, unit, reg, func):
    client = ModbusSerialClient(
        port=PORT,
        baudrate=baudrate,
        bytesize=8,
        parity=parity,
        stopbits=stopbits,
        timeout=0.5,
        retries=1,
    )
    try:
        ok = client.connect()
        if not ok:
            print(f"connect fail baud={baudrate} {parity}{stopbits}")
            return False

        rr = read_regs(client, func, reg, 1, unit)
        is_error = getattr(rr, "isError", lambda: True)()
        registers = getattr(rr, "registers", None)

        if not is_error and registers:
            print(
                f"HIT func={func:02d} baud={baudrate} {parity}{stopbits} unit={unit} reg={reg} resp={registers}"
            )
            return True

        print(
            f"miss func={func:02d} baud={baudrate} {parity}{stopbits} unit={unit} reg={reg} err={is_error} resp={registers}"
        )
        return False
    except Exception as exc:
        print(f"exc func={func:02d} baud={baudrate} {parity}{stopbits} unit={unit} reg={reg}: {exc}")
        return False
    finally:
        client.close()

print(f"scan start port={PORT}")
for baudrate in BAUDRATES:
    for parity in PARITIES:
        for stopbits in STOPBITS:
            print(f"== scan baud={baudrate} {parity}{stopbits} ==")
            for unit in UNITS:
                for reg in REGS:
                    for func in FUNCS:
                        if try_one(baudrate, parity, stopbits, unit, reg, func):
                            raise SystemExit(0)

print("no response in scanned range")
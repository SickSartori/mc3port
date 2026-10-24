"""Minimal Win32 debugger for mc3.exe: reports exceptions the process itself cannot (fail-fast, faults with a
broken stack), with registers and the game return addresses on the stack. Windows only; 64-bit Python.

  python -I tools/debug_run.py <mc3.exe> <disc.iso> [seconds] [--break 0x419C94,0x419FEA]

--break sets one-shot breakpoints (once the XBE is mapped) that print the registers when reached: put them
right after a call to see what it returned.

First-chance exceptions the runtime handles itself (privileged instructions, the game's own exceptions) are
passed on and only counted; the run stops at the first exception the process does not survive.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x2
EXCEPTION_DEBUG_EVENT = 1
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
CREATE_PROCESS_DEBUG_EVENT = 3
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
WOW64_CONTEXT_FULL = 0x10007
QUIET = {0x80000003, 0x4000001F, 0x406D1388, 0xC0000096, 0xE06D7363, 0x40010006, 0x4001000A}


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [("ExceptionCode", wt.DWORD), ("ExceptionFlags", wt.DWORD), ("ExceptionRecord", ctypes.c_void_p),
                ("ExceptionAddress", ctypes.c_void_p), ("NumberParameters", wt.DWORD),
                ("ExceptionInformation", ctypes.c_uint64 * 15)]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wt.DWORD)]


class DEBUG_EVENT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("Exception", EXCEPTION_DEBUG_INFO), ("raw", ctypes.c_byte * 160)]
    _fields_ = [("dwDebugEventCode", wt.DWORD), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD), ("u", _U)]


class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [("ContextFlags", wt.DWORD), ("Dr", wt.DWORD * 6), ("FloatSave", ctypes.c_byte * 112),
                ("SegGs", wt.DWORD), ("SegFs", wt.DWORD), ("SegEs", wt.DWORD), ("SegDs", wt.DWORD),
                ("Edi", wt.DWORD), ("Esi", wt.DWORD), ("Ebx", wt.DWORD), ("Edx", wt.DWORD), ("Ecx", wt.DWORD),
                ("Eax", wt.DWORD), ("Ebp", wt.DWORD), ("Eip", wt.DWORD), ("SegCs", wt.DWORD), ("EFlags", wt.DWORD),
                ("Esp", wt.DWORD), ("SegSs", wt.DWORD), ("ExtendedRegisters", ctypes.c_byte * 512)]


class STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


kernel32.OpenThread.restype = wt.HANDLE
kernel32.WaitForDebugEvent.argtypes = [ctypes.POINTER(DEBUG_EVENT), wt.DWORD]


def read(process, address, size):
    buffer = ctypes.create_string_buffer(size)
    done = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(process, ctypes.c_void_p(address), buffer, size, ctypes.byref(done)):
        return None
    return buffer.raw[:done.value]


def report(process, thread_id, record, first_chance):
    code = record.ExceptionCode
    print(f"\n*** exception {code:08X} at {record.ExceptionAddress or 0:08X} "
          f"({'first' if first_chance else 'second'} chance), thread {thread_id}")
    for i in range(min(record.NumberParameters, 4)):
        print(f"    parameter {i}: {record.ExceptionInformation[i]:08X}")
    thread = kernel32.OpenThread(0x1FFFFF, False, thread_id)
    context = WOW64_CONTEXT()
    context.ContextFlags = WOW64_CONTEXT_FULL
    if not kernel32.Wow64GetThreadContext(thread, ctypes.byref(context)):
        print("    cannot read the thread context")
        return
    print(f"    eip={context.Eip:08X} esp={context.Esp:08X} ebp={context.Ebp:08X} eax={context.Eax:08X} "
          f"ebx={context.Ebx:08X} ecx={context.Ecx:08X} edx={context.Edx:08X} esi={context.Esi:08X} "
          f"edi={context.Edi:08X} fs={context.SegFs:X}")
    code_bytes = read(process, context.Eip, 16)
    if code_bytes:
        print("    code at eip:", code_bytes.hex(" "))
    stack = read(process, context.Esp, 0x4000)
    if stack:
        words = struct.unpack(f"<{len(stack) // 4}I", stack)
        print("    stack:", " ".join(f"{w:08X}" for w in words[:16]))
        game = [f"{w:08X}" for w in words if 0x12000 <= w < 0x58A280][:40]
        print("    game return addresses:", " ".join(game))
    else:
        print("    stack is not readable")


def write(process, address, data):
    old = wt.DWORD()
    kernel32.VirtualProtectEx(process, ctypes.c_void_p(address), len(data), 0x40, ctypes.byref(old))
    done = ctypes.c_size_t()
    kernel32.WriteProcessMemory(process, ctypes.c_void_p(address), data, len(data), ctypes.byref(done))
    kernel32.FlushInstructionCache(process, ctypes.c_void_p(address), len(data))


def main():
    args = [a for a in sys.argv[1:]]
    breaks = []
    if "--break" in args:
        i = args.index("--break")
        breaks = [int(a, 16) for a in args[i + 1].split(",")]
        del args[i:i + 2]
    exe, iso = args[0], args[1]
    seconds = float(args[2]) if len(args) > 2 else 20
    armed = {}  # address -> original byte
    startup = STARTUPINFO()
    startup.cb = ctypes.sizeof(startup)
    info = PROCESS_INFORMATION()
    command = ctypes.create_unicode_buffer(f'"{exe}" "{iso}"')
    if not kernel32.CreateProcessW(None, command, None, None, False, DEBUG_ONLY_THIS_PROCESS, None, None,
                                   ctypes.byref(startup), ctypes.byref(info)):
        sys.exit(f"CreateProcess failed: {ctypes.get_last_error()}")
    deadline = time.time() + seconds
    counts = {}
    event = DEBUG_EVENT()
    while time.time() < deadline:
        if not kernel32.WaitForDebugEvent(ctypes.byref(event), 200):
            continue
        status = DBG_CONTINUE
        if event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT:
            record = event.u.Exception.ExceptionRecord
            first = bool(event.u.Exception.dwFirstChance)
            code = record.ExceptionCode
            counts[code] = counts.get(code, 0) + 1
            address = record.ExceptionAddress or 0
            if breaks and not armed and code == 0xC0000005 and 0xFD000000 <= record.ExceptionInformation[1] < 0xFE000000:
                for b in breaks:  # the XBE is mapped and running once the first GPU register access happens
                    armed[b] = read(info.hProcess, b, 1)
                    write(info.hProcess, b, bytes([0xCC]))
            if code in (0x80000003, 0x4000001F) and address in armed:
                thread = kernel32.OpenThread(0x1FFFFF, False, event.dwThreadId)
                context = WOW64_CONTEXT()
                context.ContextFlags = WOW64_CONTEXT_FULL
                kernel32.Wow64GetThreadContext(thread, ctypes.byref(context))
                print(f"breakpoint {address:08X}: eax={context.Eax:08X} ecx={context.Ecx:08X} edx={context.Edx:08X} "
                      f"esi={context.Esi:08X} edi={context.Edi:08X}")
                write(info.hProcess, address, armed.pop(address))
                context.Eip = address
                kernel32.Wow64SetThreadContext(thread, ctypes.byref(context))
                kernel32.CloseHandle(thread)
                status = DBG_CONTINUE
            elif code in (0x80000003, 0x4000001F):  # initial breakpoints
                status = DBG_CONTINUE
            else:
                status = DBG_EXCEPTION_NOT_HANDLED
                fatal = (not first) or code == 0xC0000409 or (record.ExceptionFlags & 1)
                # Faults on the NV2A registers are the runtime's MMIO traps, not errors.
                mmio = code == 0xC0000005 and 0xFD000000 <= record.ExceptionInformation[1] < 0xFE000000
                if not mmio:
                    counts[-1] = counts.get(-1, 0) + 1
                if fatal or (code not in QUIET and not mmio and counts[-1] <= 3):
                    report(info.hProcess, event.dwThreadId, record, first)
        elif event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT:
            print(f"\nprocess exited with code {ctypes.c_uint32(event.u.raw[0] | event.u.raw[1] << 8 | event.u.raw[2] << 16 | event.u.raw[3] << 24).value:08X}")
            break
        kernel32.ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status)
    else:
        print("\ntime limit reached, terminating")
        kernel32.TerminateProcess(info.hProcess, 1)
    print("exception counts:", {f"{k & 0xFFFFFFFF:08X}": v for k, v in counts.items() if k >= 0})


if __name__ == "__main__":
    main()

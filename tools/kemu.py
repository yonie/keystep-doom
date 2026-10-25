"""Repository paths shared by the tools."""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def below_normal():
    """Drop this process to BelowNormal (Windows) so long jobs never compete with the desktop."""
    try:
        import psutil
        psutil.Process().nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
        return
    except Exception:
        pass
    if os.name == 'nt':
        import ctypes
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x00004000)   # BELOW_NORMAL_PRIORITY_CLASS

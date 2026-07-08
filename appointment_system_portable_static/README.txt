Online Appointment System - portable static build version

1. On a computer that has MSYS2 UCRT64 gcc/g++ installed, double-click build.bat.
2. It generates appointment_server.exe using static linking to reduce missing DLL errors.
3. After build success, keep these files in the same folder:
   - appointment_server.exe
   - index.html
4. On another Windows computer, double-click appointment_server.exe.
5. Browser will open http://127.0.0.1:8080 automatically.

If Windows blocks the exe, click More info -> Run anyway.
Do not close the black console window while using the website.

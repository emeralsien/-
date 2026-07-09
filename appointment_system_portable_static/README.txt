Online Appointment System - portable static build version

1. On a computer with Visual Studio Build Tools installed, open a Developer Command Prompt and run:
   cl /EHsc /std:c++14 main.cpp sqlite3.c /link ws2_32.lib
2. Or on a computer with MinGW/g++ installed, run:
   g++ -std=c++11 main.cpp sqlite3.c -o appointment_server.exe -lws2_32
3. After build success, keep these files in the same folder:
   - appointment_server.exe
   - index.html
4. On another Windows computer, double-click appointment_server.exe.
5. Browser will open http://127.0.0.1:8080 automatically.

If Windows blocks the exe, click More info -> Run anyway.
Do not close the black console window while using the website.

Online Appointment System - Improved UI/Business Version

How to use on Windows:
1. If appointment_server.exe already exists, double-click it.
2. If exe does not exist, double-click build.bat first. It will generate appointment_server.exe.
3. Keep the black console window open. The browser will open http://127.0.0.1:8080 automatically.
4. Send the whole folder to classmates. This build script uses static linking to reduce missing-DLL problems.

Default test accounts:
User:     13311112222 / 123456
Merchant: 13800001111 / 123456

Main improvements in this version:
- One unified login/register page for both user and merchant roles.
- User dashboard and merchant dashboard are strictly separated after login.
- User side only shows user actions: book, cancel own appointment, review completed service.
- Merchant side only shows merchant actions: publish services, open appointment slots, confirm/cancel/complete orders.
- Appointment time is now created by merchants as available slots. Users can only select merchant-opened slots.
- Desktop UI redesigned with sidebar, cards, panels and responsive layout.
- Mobile UI uses page-style navigation instead of one very long page.
- Added t_slot table and slot APIs for merchant-controlled appointment time management.

@echo off
rem Builds tools\dist\ACS550-Simulator.exe: the drive simulator as one file that runs on any
rem 64-bit Windows PC without Python. Uses PlatformIO's Python to make a private build environment.
setlocal
cd /d "%~dp0"
set PY=%USERPROFILE%\.platformio\penv\Scripts\python.exe
if not exist .buildenv\Scripts\python.exe (
    "%PY%" -m venv .buildenv || goto fail
)
.buildenv\Scripts\python.exe -m pip install --quiet --disable-pip-version-check pyinstaller==6.11.1 pyserial==3.5 || goto fail
.buildenv\Scripts\python.exe -m PyInstaller --noconfirm --onefile --console --name ACS550-Simulator ^
    --distpath dist --workpath build --specpath build acs550_sim.py || goto fail
echo.
echo Built %~dp0dist\ACS550-Simulator.exe
exit /b 0
:fail
echo.
echo Build failed
exit /b 1

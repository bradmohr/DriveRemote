@echo off
rem Double-click to start the ACS550 drive simulator.
rem The COM port and drive settings are in ACS550-Simulator.ini in this folder (created on the first run).
cd /d "%~dp0.."
"%USERPROFILE%\.platformio\penv\Scripts\python.exe" tools\acs550_sim.py %*
pause

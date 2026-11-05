@echo off
title MHGU Recomp - Extract from NSP
cd /d "%~dp0"

REM Prefer MSYS2 UCRT64 Python (installed with the build tools). Avoid the broken
REM "Python312" folder on this PC that is actually Python 3.1 from 2009.
set "PY="
if exist "C:\msys64\ucrt64\bin\python.exe" set "PY=C:\msys64\ucrt64\bin\python.exe"

if not defined PY where py >nul 2>&1 && (
  for /f "delims=" %%I in ('py -3.12 -c "import sys; print(sys.executable)" 2^>nul') do set "PY=%%I"
)
if not defined PY where py >nul 2>&1 && (
  for /f "delims=" %%I in ('py -3.11 -c "import sys; print(sys.executable)" 2^>nul') do set "PY=%%I"
)
if not defined PY where py >nul 2>&1 && (
  for /f "delims=" %%I in ('py -3.10 -c "import sys; print(sys.executable)" 2^>nul') do set "PY=%%I"
)

if not defined PY (
  echo Need Python 3.10 or newer.
  echo Easiest: install MSYS2 UCRT64 python, or from https://www.python.org/
  echo   C:\msys64\usr\bin\bash -lc "pacman -S --noconfirm mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-tk"
  pause
  exit /b 1
)

echo Using: %PY%
"%PY%" -c "import sys; assert sys.version_info >= (3, 10), sys.version"
if errorlevel 1 (
  echo That Python is too old.
  pause
  exit /b 1
)

"%PY%" -c "import tkinter" 2>nul
if errorlevel 1 (
  echo Installing tkinter for MSYS2 Python...
  C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-tk" 
)

"%PY%" tools\extract_mhgu_nsp.py
if errorlevel 1 pause

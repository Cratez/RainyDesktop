@echo off
rem Signs raindesk.exe with your code-signing certificate, so you can tell
rem later whether it has been modified. CMake runs this after every build.
rem
rem   sign.bat [exe]          sign it, then check it (default: build\Release\raindesk.exe)
rem   sign.bat /check [exe]   only check it: signed by your certificate and unchanged since?
rem
rem The certificate is found by its thumbprint in your personal store
rem (Cert:\CurrentUser\My). Set RAINDESK_SIGN_THUMBPRINT to use a different one.
rem On a machine without the certificate, or while raindesk is running and
rem holds the exe open, the exe is left unsigned and the build carries on.
setlocal
set "THUMB=A4F374779E9E9B90A3F74DAB29204169BDF9101F"
if defined RAINDESK_SIGN_THUMBPRINT set "THUMB=%RAINDESK_SIGN_THUMBPRINT%"

set "MODE=sign"
if /i "%~1"=="/check" (
  set "MODE=check"
  shift /1
)
set "EXE=%~f1"
if "%~1"=="" set "EXE=%~dp0build\Release\raindesk.exe"
if not exist "%EXE%" goto missing
if "%MODE%"=="check" goto check

rem CMake runs this on every build, whether or not the exe was relinked. A
rem freshly linked exe is unsigned; one already signed with your certificate
rem and unchanged needs nothing. Never sign over a file that was changed after
rem it was signed, or signed by someone else: that would vouch for it.
call :verify >nul
set "STATE=%errorlevel%"
if "%STATE%"=="0" (
  echo sign: already signed with your certificate and unchanged.
  exit /b 0
)
if not "%STATE%"=="2" (
  call :verify
  echo sign: not signing over that. Delete the exe and build again.
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path -LiteralPath ('Cert:\CurrentUser\My\' + $env:THUMB)) { exit 0 } else { exit 1 }"
if errorlevel 1 (
  echo sign: certificate %THUMB% is not in your personal store, so the exe was left unsigned.
  exit /b 0
)

rem signtool comes with the Windows SDK. It is on PATH in a Developer Command
rem Prompt (build.bat sets one up); otherwise use the newest one installed.
set "SIGNTOOL="
for /f "delims=" %%p in ('where signtool.exe 2^>nul') do if not defined SIGNTOOL set "SIGNTOOL=%%p"
if not defined SIGNTOOL for /f "delims=" %%d in ('dir /b /ad /on "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do (
  if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\signtool.exe" set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\%%d\x64\signtool.exe"
)
if not defined SIGNTOOL if exist "%ProgramFiles(x86)%\Windows Kits\10\App Certification Kit\signtool.exe" set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\App Certification Kit\signtool.exe"
if not defined SIGNTOOL (
  echo sign: signtool.exe not found. It comes with the Windows SDK, part of the C++ workload.
  exit /b 1
)

rem No timestamp: it only keeps a signature trusted after the certificate
rem expires, which doesn't matter for checking the file is unchanged, and
rem it would need the network on every build.
rem A virus scan of the new file can hold it for a moment, so try a few times.
rem signtool's output is kept out of the build log until the last try,
rem because MSBuild fails the build on any line that looks like an error.
set "LOG=%TEMP%\raindesk-sign-%RANDOM%.log"
set /a TRIES=0
:retry
"%SIGNTOOL%" sign /sha1 %THUMB% /fd SHA256 "%EXE%" >"%LOG%" 2>&1
if not errorlevel 1 goto signed
set /a TRIES+=1
if %TRIES% lss 5 (
  ping -n 2 127.0.0.1 >nul
  goto retry
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { [IO.File]::Open($env:EXE, 'Open', 'ReadWrite', 'None').Close(); exit 0 } catch { exit 1 }"
if errorlevel 1 goto inuse
type "%LOG%"
del "%LOG%" 2>nul
echo sign: signing failed.
exit /b 1

:signed
del "%LOG%" 2>nul
echo sign: signed %EXE%

:check
call :verify
exit /b %errorlevel%

:verify
rem A self-signed certificate isn't trusted by Windows unless you add it to
rem Trusted Root, so a good file shows Valid or UnknownError (untrusted root);
rem either way Windows has checked the file's hash. Anyone can strip a
rem signature and sign with their own certificate, so the signer must be yours.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s = Get-AuthenticodeSignature -LiteralPath $env:EXE; if ($s.Status -eq 'NotSigned') { Write-Host ($env:EXE + ' is NOT signed.'); exit 2 }; if ($s.Status -eq 'HashMismatch') { Write-Host ($env:EXE + ' has been MODIFIED since it was signed.'); exit 3 }; if ($s.Status -ne 'Valid' -and $s.Status -ne 'UnknownError') { Write-Host ('Could not check the signature: ' + $s.StatusMessage); exit 4 }; if ($s.SignerCertificate.Thumbprint -ne $env:THUMB) { Write-Host ($env:EXE + ' is signed by someone else: ' + $s.SignerCertificate.Subject); exit 5 }; Write-Host ('OK: ' + $env:EXE + ' is signed with your certificate and unchanged since.'); exit 0"
exit /b %errorlevel%

:missing
echo sign: %EXE% not found.
exit /b 1

:inuse
del "%LOG%" 2>nul
echo sign: %EXE% is in use, probably because raindesk is running, so it was left unsigned.
echo sign: quit raindesk (Ctrl+Alt+Q) and build again to sign it.
exit /b 0

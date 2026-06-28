@echo off
REM ============================================================
REM  Post-build script - RXDK Xbox XBE builder  (Flycast port)
REM  Adapted from the Zelda ALTTP X build script.
REM  Intended to be invoked as the project's PostBuildEvent, so the
REM  $(OutDir)/$(TargetFileName)/$(ProjectName)/$(ProjectDir) MSBuild
REM  macros are expanded by MSBuild before this runs.
REM ============================================================

REM --- CONFIGURE THESE ---
SET TITLE_NAME=Flycast X
REM  Title ID: "TR" (0x5452) publisher space + DC01 = Dreamcast emulator #1
SET TITLE_ID=0x5452DC01
SET TITLE_IMAGE=$(ProjectDir)titleimage.png
SET SAVE_IMAGE=$(ProjectDir)saveimage.png
REM  Emulator needs a deeper stack than a normal homebrew title (dynarec +
REM  recursive HW emulation call chains). 128 KB to be safe.
SET STACK_SIZE=131072
REM -----------------------

REM Derive TDATA folder name from title ID (strip 0x prefix)
SET TDATA_ID=%TITLE_ID:0x=%

REM --- Patch subsystem flag ---
"$(RXDK_LIBS)bin\patchSubsystem.exe" -i="$(OutDir)$(TargetFileName)" -s=0x000E

REM --- Optional title/save images (only if the PNGs exist yet) ---
REM  These accumulate the matching imagebld args so the XBE still builds
REM  cleanly before any art has been made.
SET TITLEIMG_ARG=
SET SAVEIMG_ARG=

IF EXIST "%TITLE_IMAGE%" (
    COPY /Y "%TITLE_IMAGE%" "$(OutDir)titleimage.png" >NUL
    ECHO Texture MyTex > "$(OutDir)titleimage.rdf"
    ECHO { >> "$(OutDir)titleimage.rdf"
    ECHO Source titleimage.png >> "$(OutDir)titleimage.rdf"
    ECHO Format D3DFMT_DXT1 >> "$(OutDir)titleimage.rdf"
    ECHO Width 128 >> "$(OutDir)titleimage.rdf"
    ECHO Height 128 >> "$(OutDir)titleimage.rdf"
    ECHO } >> "$(OutDir)titleimage.rdf"
    "$(RXDK_LIBS)bin\bundler.exe" "$(OutDir)titleimage.rdf" -o "$(OutDir)titleimage.xbx"
    DEL /q "$(OutDir)titleimage.rdf"
    SET TITLEIMG_ARG=/titleimage:"$(OutDir)titleimage.xbx"
) ELSE (
    ECHO [build_xbe] No titleimage.png - skipping title image.
)

IF EXIST "%SAVE_IMAGE%" (
    COPY /Y "%SAVE_IMAGE%" "$(OutDir)saveimage.png" >NUL
    ECHO Texture MySaveTex > "$(OutDir)saveimage.rdf"
    ECHO { >> "$(OutDir)saveimage.rdf"
    ECHO Source saveimage.png >> "$(OutDir)saveimage.rdf"
    ECHO Format D3DFMT_DXT1 >> "$(OutDir)saveimage.rdf"
    ECHO Width 64 >> "$(OutDir)saveimage.rdf"
    ECHO Height 64 >> "$(OutDir)saveimage.rdf"
    ECHO } >> "$(OutDir)saveimage.rdf"
    "$(RXDK_LIBS)bin\bundler.exe" "$(OutDir)saveimage.rdf" -o "$(OutDir)saveimage.xbx"
    DEL /q "$(OutDir)saveimage.rdf"
    SET SAVEIMG_ARG=/defaultsaveimage:"$(OutDir)saveimage.xbx"
) ELSE (
    ECHO [build_xbe] No saveimage.png - skipping save image.
)

REM --- Build XBE ---
"$(RXDK_LIBS)bin\imagebld.exe" ^
    /in:"$(OutDir)$(TargetFileName)" ^
    /out:"$(OutDir)$(TargetName).xbe" ^
    /nologo ^
    /stack:%STACK_SIZE% ^
    /debug ^
    /nolibwarn ^
    /testid:"%TITLE_ID%" ^
    /testname:"%TITLE_NAME%" ^
    %TITLEIMG_ARG% ^
    %SAVEIMG_ARG%

REM --- Assemble build folder ---
REM  Layout: default.xbe at root, assets in Media\ subfolder.
MKDIR "$(OutDir)Build\$(ProjectName)\Media" >NUL 2>&1
COPY /Y "$(OutDir)$(TargetName).xbe" "$(OutDir)Build\$(ProjectName)\default.xbe" >NUL
XCOPY /C /Q /I /E /S /Y /F "$(ProjectDir)Media\*" "$(OutDir)Build\$(ProjectName)\Media\" >NUL 2>NUL

REM  Dreamcast firmware / disc assets (copied into Media if present).
REM    dc_flash.bin  - DC flash (region/date settings); auto-created if absent.
REM    dc_boot.bin   - real DC BIOS (optional; reios HLE BIOS works without it).
REM    game.*        - a .gdi/.cdi/.chd disc image to auto-boot (optional).
IF EXIST "$(ProjectDir)dc_flash.bin" COPY /Y "$(ProjectDir)dc_flash.bin" "$(OutDir)Build\$(ProjectName)\Media\dc_flash.bin" >NUL
IF EXIST "$(ProjectDir)dc_boot.bin"  COPY /Y "$(ProjectDir)dc_boot.bin"  "$(OutDir)Build\$(ProjectName)\Media\dc_boot.bin"  >NUL

REM --- Pack XISO ---
MKDIR "$(OutDir)XISO" >NUL 2>&1
"$(RXDK_LIBS)bin\xdvdfs.exe" pack "$(OutDir)Build\$(ProjectName)" "$(OutDir)XISO\$(ProjectName).iso"

ECHO.
ECHO ============================================================
ECHO  Title:    %TITLE_NAME%
ECHO  Title ID: %TITLE_ID%
ECHO  TDATA:    E:\TDATA\%TDATA_ID%
ECHO  UDATA:    E:\UDATA\%TDATA_ID%
ECHO  Build:    $(OutDir)Build\$(ProjectName)
ECHO  ISO:      $(OutDir)XISO\$(ProjectName).iso
ECHO ============================================================
ECHO.

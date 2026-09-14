Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
Name "PcTool"
VIProductVersion "0.1.2.0"
VIAddVersionKey /LANG=2052 "ProductName" "PcTool"
VIAddVersionKey /LANG=2052 "ProductVersion" "0.1.2"
VIAddVersionKey /LANG=2052 "FileVersion" "0.1.2.0"
VIAddVersionKey /LANG=2052 "FileDescription" "PcTool Setup"
OutFile "${OUTPUT}"
InstallDir "$PROGRAMFILES64\PcTool"
InstallDirRegKey HKLM "Software\PcToolInstaller" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
!define MUI_ICON "..\Resources\PcTool.ico"
!define MUI_UNICON "..\Resources\PcTool.ico"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "license.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ValidateDirectory
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!define MUI_UNCONFIRMPAGE_TEXT_TOP "确认卸载后，将终止 PcTool 及内置 7-Zip 的全部任务，包括录屏、GIF、压缩和解压。未完成结果可能不可用。必要时会自动重启资源管理器，以释放文件并清除右键菜单。"
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"
Var OldDirectory
Var UninstallFailed

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "PcTool 安装包需要 64 位 Windows。"
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  ReadRegStr $OldDirectory HKLM "Software\PcToolInstaller" "InstallDir"
  ${If} $OldDirectory != ""
    StrCpy $INSTDIR $OldDirectory
  ${EndIf}
  ReadRegStr $0 HKLM "Software\Microsoft\Windows NT\CurrentVersion" "CurrentBuildNumber"
  ${If} $0 < 19041
    MessageBox MB_ICONSTOP "PcTool 需要 Windows 10 2004 或更新版本。"
    Abort
  ${EndIf}
FunctionEnd

Function ValidateDirectory
  ${GetRoot} "$INSTDIR" $0
  ${If} $INSTDIR == $0
  ${OrIf} $INSTDIR == "$0\"
    MessageBox MB_ICONSTOP "请选择独立的安装文件夹，不能直接安装到磁盘根目录。"
    Abort
  ${EndIf}
  ${If} $OldDirectory != ""
  ${AndIf} $OldDirectory != $INSTDIR
    MessageBox MB_ICONSTOP "升级请使用原目录：$OldDirectory。要更换位置，请先卸载旧版。"
    Abort
  ${EndIf}
FunctionEnd

; Wait on the process handle, not just its HWND: recording finalization happens during WM_DESTROY.
!macro ClosePcTool PREFIX
Function ${PREFIX}ClosePcTool
  FindWindow $0 "PcTool.BackgroundController"
  ${If} $0 P<> 0
    System::Call 'user32::GetWindowThreadProcessId(p r0, *i .r1)'
    System::Call 'kernel32::OpenProcess(i 0x100000, i 0, i r1) p.r2'
    ${If} $2 P= 0
      MessageBox MB_ICONSTOP "无法等待 PcTool 安全退出，请先手动退出后重试。" /SD IDOK
      Abort
    ${EndIf}
    System::Call 'user32::PostMessageW(p r0, i 0x10, p 0, p 0)'
    DetailPrint "正在等待 PcTool 完成录制封装并退出……"
    StrCpy $4 0
    ${Do}
      System::Call 'kernel32::WaitForSingleObject(p r2, i 1000) i.r3'
      ${If} $3 = 0
        ${ExitDo}
      ${EndIf}
      IntOp $4 $4 + 1
      ${If} $4 >= 120
        System::Call 'kernel32::CloseHandle(p r2)'
        MessageBox MB_ICONSTOP "PcTool 尚未结束后台任务，请稍后重试。未强制结束录制。" /SD IDOK
        Abort
      ${EndIf}
    ${Loop}
    System::Call 'kernel32::CloseHandle(p r2)'
  ${EndIf}
FunctionEnd
!macroend
!insertmacro ClosePcTool ""

; Explorer may keep the extension mapped. Rename that DLL, install the new one,
; and let Windows remove the old mapped copy after reboot (as the upstream installer does).
!macro ReplaceShellDll DLL
  IfFileExists "$INSTDIR\modules\archive\${DLL}" 0 shell_done_${DLL}
  ClearErrors
  Delete "$INSTDIR\modules\archive\${DLL}"
  ${If} ${Errors}
    System::Call 'kernel32::GetTickCount() i.r5'
    ClearErrors
    Rename "$INSTDIR\modules\archive\${DLL}" "$INSTDIR\modules\archive\${DLL}.$5.old"
    ${If} ${Errors}
      MessageBox MB_ICONSTOP "无法更新 $INSTDIR\modules\archive\${DLL}。请关闭正在使用它的窗口后重试。" /SD IDOK
      Abort
    ${EndIf}
    Delete /REBOOTOK "$INSTDIR\modules\archive\${DLL}.$5.old"
  ${EndIf}
  shell_done_${DLL}:
!macroend

; Keep a per-value backup of pre-existing 7-Zip integration; upgrades keep the original backup.
!macro RegisterValue VIEW ID ROOT KEY VALUE DATA
  SetRegView ${VIEW}
  ReadRegStr $0 HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Saved"
  ${If} $0 != "1"
    ClearErrors
    ReadRegStr $1 ${ROOT} "${KEY}" "${VALUE}"
    ${If} ${Errors}
      WriteRegStr HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Exists" "0"
    ${Else}
      WriteRegStr HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Exists" "1"
      WriteRegStr HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Data" "$1"
    ${EndIf}
    WriteRegStr HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Saved" "1"
  ${EndIf}
  WriteRegStr ${ROOT} "${KEY}" "${VALUE}" "${DATA}"
!macroend

!macro RestoreValue VIEW ID ROOT KEY VALUE DATA
  SetRegView ${VIEW}
  ReadRegStr $0 ${ROOT} "${KEY}" "${VALUE}"
  ${If} $0 == "${DATA}"
    ReadRegStr $1 HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Exists"
    ${If} $1 == "1"
      ReadRegStr $2 HKLM "Software\PcToolInstaller\SevenZip\${ID}" "Data"
      WriteRegStr ${ROOT} "${KEY}" "${VALUE}" "$2"
    ${Else}
      DeleteRegValue ${ROOT} "${KEY}" "${VALUE}"
      DeleteRegKey /ifempty ${ROOT} "${KEY}"
    ${EndIf}
  ${EndIf}
  ; Preserve recovery information until the entire uninstall succeeds.
!macroend

!define CLSID "{23170F69-40C1-278A-1000-000100020000}"
!define CLASSES "Software\Classes"
!macro ShellValues ACTION VIEW DLL
  !insertmacro ${ACTION} ${VIEW} Class HKLM "${CLASSES}\CLSID\${CLSID}" "" "7-Zip Shell Extension"
  !insertmacro ${ACTION} ${VIEW} Thread HKLM "${CLASSES}\CLSID\${CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  !insertmacro ${ACTION} ${VIEW} Inproc HKLM "${CLASSES}\CLSID\${CLSID}\InprocServer32" "" "$INSTDIR\modules\archive\${DLL}"
  !insertmacro ${ACTION} ${VIEW} Approved HKLM "Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" "${CLSID}" "7-Zip Shell Extension"
  !insertmacro ${ACTION} ${VIEW} Files HKLM "${CLASSES}\*\shellex\ContextMenuHandlers\7-Zip" "" "${CLSID}"
  !insertmacro ${ACTION} ${VIEW} Directory HKLM "${CLASSES}\Directory\shellex\ContextMenuHandlers\7-Zip" "" "${CLSID}"
  !insertmacro ${ACTION} ${VIEW} Folder HKLM "${CLASSES}\Folder\shellex\ContextMenuHandlers\7-Zip" "" "${CLSID}"
  !insertmacro ${ACTION} ${VIEW} DirectoryDrag HKLM "${CLASSES}\Directory\shellex\DragDropHandlers\7-Zip" "" "${CLSID}"
  !insertmacro ${ACTION} ${VIEW} DriveDrag HKLM "${CLASSES}\Drive\shellex\DragDropHandlers\7-Zip" "" "${CLSID}"
!macroend

Section "PcTool 与完整 7-Zip" Main
  Call ValidateDirectory
  Call ClosePcTool
  InitPluginsDir
  File /oname=$PLUGINSDIR\manage-data.ps1 "manage-data.ps1"
  File /oname=$PLUGINSDIR\package-files.json "${STAGE}\.pctool-package-files.json"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-data.ps1" -Action Record -Root "$INSTDIR" -Manifest "$PLUGINSDIR\package-files.json"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONSTOP "无法保存安装文件清单，安装未完成。" /SD IDOK
    Abort
  ${EndIf}
  !insertmacro ReplaceShellDll "7-zip.dll"
  !insertmacro ReplaceShellDll "7-zip32.dll"
  ; Files currently used by an archive job must be released by that job. No force termination.
  SetOverwrite on
  !include "${MANIFEST}\install-files.nsh"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\manage-data.ps1" -Action Install -Root "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONSTOP "无法设置 Data/Cache 写入权限，安装未完成。"
    Abort
  ${EndIf}
  !insertmacro ShellValues RegisterValue 64 "7-zip.dll"
  !insertmacro ShellValues RegisterValue 32 "7-zip32.dll"
  !insertmacro RegisterValue 64 Path HKLM "Software\7-Zip" "Path" "$INSTDIR\modules\archive\"
  !insertmacro RegisterValue 64 Path64 HKLM "Software\7-Zip" "Path64" "$INSTDIR\modules\archive\"
  !insertmacro RegisterValue 32 Path HKLM "Software\7-Zip" "Path" "$INSTDIR\modules\archive\"
  !insertmacro RegisterValue 32 Path64 HKLM "Software\7-Zip" "Path64" "$INSTDIR\modules\archive\"
  !insertmacro RegisterValue 64 AppPath HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe" "" "$INSTDIR\modules\archive\7zFM.exe"
  !insertmacro RegisterValue 64 AppDirectory HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe" "Path" "$INSTDIR\modules\archive\"
  SetRegView 64
  WriteRegStr HKLM "Software\PcToolInstaller" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "DisplayName" "PcTool（含 7-Zip）"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "DisplayVersion" "0.1.2"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "DisplayIcon" "$INSTDIR\PcTool.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "NoRepair" 1
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\PcTool"
  SetOutPath "$INSTDIR"
  CreateShortcut "$SMPROGRAMS\PcTool\PcTool.lnk" "$INSTDIR\PcTool.exe"
  CreateShortcut "$DESKTOP\PcTool.lnk" "$INSTDIR\PcTool.exe" "" "$INSTDIR\PcTool.exe"
  CreateShortcut "$SMPROGRAMS\PcTool\压缩管理（7-Zip）.lnk" "$INSTDIR\PcTool.exe" "--archive" "$INSTDIR\modules\archive\7zFM.exe"
  CreateShortcut "$SMPROGRAMS\PcTool\7-Zip 帮助.lnk" "$INSTDIR\modules\archive\7-zip.chm"
  CreateShortcut "$SMPROGRAMS\PcTool\卸载 PcTool.lnk" "$INSTDIR\Uninstall.exe"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Uninstall"
  StrCpy $UninstallFailed 0
  DetailPrint "正在停止本次安装的 PcTool 和内置 7-Zip 任务……"
  nsExec::ExecToLog '"$INSTDIR\PcToolUninstallHelper.exe" stop "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "本次安装的进程尚未全部退出（错误 $0），已停止文件删除。请查看详细记录后重试卸载。" /SD IDOK
    Abort
  ${EndIf}
  ; Only restore shell registration while our DLL still owns the CLSID.
  SetRegView 64
  ReadRegStr $9 HKLM "${CLASSES}\CLSID\${CLSID}\InprocServer32" ""
  ${If} $9 == "$INSTDIR\modules\archive\7-zip.dll"
    !insertmacro ShellValues RestoreValue 64 "7-zip.dll"
  ${EndIf}
  SetRegView 32
  ReadRegStr $9 HKLM "${CLASSES}\CLSID\${CLSID}\InprocServer32" ""
  ${If} $9 == "$INSTDIR\modules\archive\7-zip32.dll"
    !insertmacro ShellValues RestoreValue 32 "7-zip32.dll"
  ${EndIf}
  !insertmacro RestoreValue 64 Path HKLM "Software\7-Zip" "Path" "$INSTDIR\modules\archive\"
  !insertmacro RestoreValue 64 Path64 HKLM "Software\7-Zip" "Path64" "$INSTDIR\modules\archive\"
  !insertmacro RestoreValue 32 Path HKLM "Software\7-Zip" "Path" "$INSTDIR\modules\archive\"
  !insertmacro RestoreValue 32 Path64 HKLM "Software\7-Zip" "Path64" "$INSTDIR\modules\archive\"
  !insertmacro RestoreValue 64 AppPath HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe" "" "$INSTDIR\modules\archive\7zFM.exe"
  !insertmacro RestoreValue 64 AppDirectory HKLM "Software\Microsoft\Windows\CurrentVersion\App Paths\7zFM.exe" "Path" "$INSTDIR\modules\archive\"
  SetRegView 64
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "PcTool"
  ${If} $0 == '$\"$INSTDIR\PcTool.exe$\" --background'
    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "PcTool"
  ${EndIf}
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  nsExec::ExecToLog '"$INSTDIR\PcToolUninstallHelper.exe" release-shell "$INSTDIR"'
  Pop $0
  ${If} $0 == 3010
    DetailPrint "仍有扩展占用待复查，将在文件删除重试后确认是否需要重启。"
  ${ElseIf} $0 != 0
    StrCpy $UninstallFailed 1
    DetailPrint "资源管理器释放或恢复未完成，错误 $0。"
  ${EndIf}
  ; Confirm no owned task appeared during shell recovery, before deleting its files.
  nsExec::ExecToLog '"$INSTDIR\PcToolUninstallHelper.exe" stop "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "占用复查发现本次安装仍有进程未退出（错误 $0），已停止文件删除。请重试卸载。" /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\manage-data.ps1" -Action Uninstall -Root "$INSTDIR"'
  Pop $0
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 != 0
    StrCpy $UninstallFailed 1
  ${EndIf}
  Delete "$SMPROGRAMS\PcTool\PcTool.lnk"
  Delete "$DESKTOP\PcTool.lnk"
  Delete "$SMPROGRAMS\PcTool\压缩管理（7-Zip）.lnk"
  Delete "$SMPROGRAMS\PcTool\7-Zip 帮助.lnk"
  Delete "$SMPROGRAMS\PcTool\卸载 PcTool.lnk"
  RMDir "$SMPROGRAMS\PcTool"
  ; Detect an actual failed rollback, rather than reporting stale menu registration as success.
  SetRegView 64
  ReadRegStr $0 HKLM "${CLASSES}\CLSID\${CLSID}\InprocServer32" ""
  ${If} $0 == "$INSTDIR\modules\archive\7-zip.dll"
    StrCpy $UninstallFailed 1
    DetailPrint "64 位右键扩展注册仍指向本次安装。"
  ${EndIf}
  SetRegView 32
  ReadRegStr $0 HKLM "${CLASSES}\CLSID\${CLSID}\InprocServer32" ""
  ${If} $0 == "$INSTDIR\modules\archive\7-zip32.dll"
    StrCpy $UninstallFailed 1
    DetailPrint "32 位右键扩展注册仍指向本次安装。"
  ${EndIf}
  ${If} $UninstallFailed != 0
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "部分项目清理失败，请查看上方详细记录。已保留卸载器和清理信息，可重新运行卸载；未报告清理完成。" /SD IDOK
    Abort
  ${EndIf}
  ; A deferred removal is unfinished. Keep the retry tools and registration.
  IfRebootFlag 0 uninstall_finalize
    SetErrorLevel 3010
    DetailPrint "仍有文件被占用，已保留卸载器和清理信息。释放占用后可再次运行卸载器重试；详情列出了延期删除的文件。"
    Goto uninstall_end
  uninstall_finalize:
  ClearErrors
  Delete /REBOOTOK "$INSTDIR\manage-data.ps1"
  Delete /REBOOTOK "$INSTDIR\PcToolUninstallHelper.exe"
  ${If} ${Errors}
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "卸载辅助文件清理失败，未完成卸载。请查看详细记录。" /SD IDOK
    Abort
  ${EndIf}
  Delete /REBOOTOK "$INSTDIR\.pctool-package-files.json"
  Delete /REBOOTOK "$INSTDIR\.pctool-managed-files.json"
  Delete /REBOOTOK "$INSTDIR\.pctool-uninstalling"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  ; Remove the root after deferred children; unrelated files still prevent removal.
  IfRebootFlag 0 uninstall_root_now
  RMDir /REBOOTOK "$INSTDIR"
  Goto uninstall_root_done
  uninstall_root_now:
    RMDir "$INSTDIR"
  uninstall_root_done:
  SetRegView 32
  DeleteRegKey HKLM "Software\PcToolInstaller"
  SetRegView 64
  DeleteRegKey HKLM "Software\PcToolInstaller"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  IfRebootFlag 0 uninstall_complete
    SetErrorLevel 3010
    DetailPrint "部分文件已安排在重启后删除；需要重启才能完成清理。"
    Goto uninstall_end
  uninstall_complete:
    SetErrorLevel 0
    DetailPrint "PcTool 受管文件与系统注册已清理；用户自行保存的无关文件予以保留。"
  uninstall_end:
SectionEnd

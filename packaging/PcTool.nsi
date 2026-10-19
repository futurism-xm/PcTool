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
Var FinishText
Var UninstallFinishText
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "license.txt"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ValidateDirectory
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "$FinishText"
!insertmacro MUI_PAGE_FINISH
!define MUI_UNCONFIRMPAGE_TEXT_TOP "确认卸载后，将终止 PcTool 及内置 7-Zip 的全部任务，包括录屏、GIF、压缩和解压。未完成结果可能不可用。被其他程序占用的文件会在释放后自动清理，无需重启电脑或资源管理器。"
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "$UninstallFinishText"
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"
Var OldDirectory
Var InstallMutex
Var InstallMutexHeld
Var CleanupPending
!ifndef PCTOOL_CLEANUP_NAMESPACE
!define PCTOOL_CLEANUP_NAMESPACE "PcTool"
!endif

Function .onInit
  StrCpy $FinishText "PcTool 已安装完成。"

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

; The installer and the SYSTEM retry worker serialize all changes to this root.
!macro PrepareTransaction
  InitPluginsDir
  File /oname=$PLUGINSDIR\manage-data.ps1 "manage-data.ps1"
  File /oname=$PLUGINSDIR\cleanup-jobs.ps1 "cleanup-jobs.ps1"
  File /oname=$PLUGINSDIR\retry-cleanup.ps1 "retry-cleanup.ps1"
  File /oname=$PLUGINSDIR\shell-integration.ps1 "shell-integration.ps1"
  File /oname=$PLUGINSDIR\lifecycle.ps1 "lifecycle.ps1"
  File /oname=$PLUGINSDIR\PcToolUninstallHelper.exe "${STAGE}\PcToolUninstallHelper.exe"
  nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-data.ps1" -Action LockName -Root "$INSTDIR" -CleanupNamespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  Pop $1
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法验证安装目录或初始化互斥锁。操作未完成。" /SD IDOK
    Abort
  ${EndIf}
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w r1) p.s'
  Pop $InstallMutex
  ${If} $InstallMutex P= 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法创建安装目录互斥锁，请稍后重试。" /SD IDOK
    Abort
  ${EndIf}
  System::Call 'kernel32::WaitForSingleObject(p $InstallMutex, i 30000) i.r0'
  ${If} $0 != 0
  ${AndIf} $0 != 128
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "同一目录正在安装、卸载或清理，请稍后重试。" /SD IDOK
    Abort
  ${EndIf}
  StrCpy $InstallMutexHeld 1
!macroend
!macro FinishTransaction
  ${If} $InstallMutexHeld == 1
    System::Call 'kernel32::ReleaseMutex(p $InstallMutex)'
    StrCpy $InstallMutexHeld 0
  ${EndIf}
  ${If} $InstallMutex P<> 0
    System::Call 'kernel32::CloseHandle(p $InstallMutex)'
    StrCpy $InstallMutex 0
  ${EndIf}
!macroend
!macro StopOwnedTasks
  nsExec::ExecToLog '"$PLUGINSDIR\PcToolUninstallHelper.exe" stop "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "本次安装的进程尚未全部退出（错误 $0）。已停止操作，请查看详细记录后重试。" /SD IDOK
    Abort
  ${EndIf}
!macroend

; A mapped extension can be renamed without closing its host. Install registers
; these exact retired copies with the same identity-bound cleanup mechanism.
!macro ReplaceShellDll DLL
  IfFileExists "$INSTDIR\modules\archive\${DLL}" 0 shell_done_${DLL}
  ClearErrors
  Delete "$INSTDIR\modules\archive\${DLL}"
  ${If} ${Errors}
    System::Call 'kernel32::GetTickCount() i.r5'
    ; NSIS integers are signed. Keep the retired-file suffix decimal even on
    ; machines with more than 24 days of uptime, and never reuse an old name.
    IntFmt $5 "%u" $5
    shell_unique_${DLL}:
    IfFileExists "$INSTDIR\modules\archive\${DLL}.$5.old" 0 shell_rename_${DLL}
    IntOp $5 $5 + 1
    IntFmt $5 "%u" $5
    Goto shell_unique_${DLL}
    shell_rename_${DLL}:
    ClearErrors
    Rename "$INSTDIR\modules\archive\${DLL}" "$INSTDIR\modules\archive\${DLL}.$5.old"
    ${If} ${Errors}
      !insertmacro FinishTransaction
    SetErrorLevel 1
      MessageBox MB_ICONSTOP "无法更新 $INSTDIR\modules\archive\${DLL}。请关闭正在使用它的窗口后重试。" /SD IDOK
      Abort
    ${EndIf}
  ${EndIf}
  shell_done_${DLL}:
!macroend
Section "PcTool 与完整 7-Zip" Main
  Call ValidateDirectory
  !insertmacro PrepareTransaction
  CreateDirectory "$INSTDIR"
  File /oname=$PLUGINSDIR\package-files.json "${STAGE}\.pctool-package-files.json"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-data.ps1" -Action Record -Root "$INSTDIR" -Manifest "$PLUGINSDIR\package-files.json" -CleanupNamespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法准备安装或处理旧卸载记录，安装未完成。请查看详细记录后重试。" /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog '"$PLUGINSDIR\PcToolUninstallHelper.exe" mark "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法设置升级状态，安装未完成。" /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\lifecycle.ps1" -Action RemoveStartup -Root "$INSTDIR" -Namespace "${PCTOOL_CLEANUP_NAMESPACE}" -LegacyOnly'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "旧版自启迁移失败，已保留重试入口。" /SD IDOK
    Abort
  ${EndIf}
  !insertmacro StopOwnedTasks
  !insertmacro ReplaceShellDll "7-zip.dll"
  !insertmacro ReplaceShellDll "7-zip32.dll"
  ; The helper has verified that this installation's archive jobs have stopped.
  ClearErrors
  SetOverwrite on
  !include "${MANIFEST}\install-files.nsh"
  ${If} ${Errors}
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "安装文件写入失败，安装未完成。请检查目录权限和磁盘空间后重试。" /SD IDOK
    Abort
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\modules\archive\7-zip.dll"
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "7-Zip 64 位右键扩展未正确安装，安装未完成。" /SD IDOK
    Abort
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\modules\archive\7-zip32.dll"
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "7-Zip 32 位右键扩展未正确安装，安装未完成。" /SD IDOK
    Abort
  ${EndIf}
  ClearErrors
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法写入卸载器，安装未完成。请检查目录权限和磁盘空间后重试。" /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-data.ps1" -Action Install -Root "$INSTDIR" -CleanupNamespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  ${If} $0 != 0
  ${AndIf} $0 != 2
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法完成权限设置或旧文件清理登记，安装未完成。请查看详细记录后重试。" /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\shell-integration.ps1" -Action Register -Root "$INSTDIR" -Namespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "7-Zip 菜单注册失败，已尝试恢复本次注册改动。安装未完成，请查看详细记录。" /SD IDOK
    Abort
  ${EndIf}
  ClearErrors
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
  ${If} ${Errors}
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法写入安装和卸载注册，安装未完成。已保留安装目录中的卸载器。" /SD IDOK
    Abort
  ${EndIf}
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "InstallLocation"
  ${If} $0 != "$INSTDIR"
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "安装注册读回验证失败，安装未完成。" /SD IDOK
    Abort
  ${EndIf}
  ClearErrors
  CreateDirectory "$SMPROGRAMS\PcTool"
  SetOutPath "$INSTDIR"
  CreateShortcut "$SMPROGRAMS\PcTool\PcTool.lnk" "$INSTDIR\PcTool.exe"
  CreateShortcut "$DESKTOP\PcTool.lnk" "$INSTDIR\PcTool.exe" "" "$INSTDIR\PcTool.exe"
  CreateShortcut "$SMPROGRAMS\PcTool\压缩管理（7-Zip）.lnk" "$INSTDIR\PcTool.exe" "--archive" "$INSTDIR\modules\archive\7zFM.exe"
  CreateShortcut "$SMPROGRAMS\PcTool\7-Zip 帮助.lnk" "$INSTDIR\modules\archive\7-zip.chm"
  CreateShortcut "$SMPROGRAMS\PcTool\卸载 PcTool.lnk" "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "快捷方式创建失败，请检查目录权限后重新运行安装器。" /SD IDOK
    Abort
  ${EndIf}
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  !insertmacro FinishTransaction
SectionEnd

Function un.onInit
  StrCpy $UninstallFinishText "PcTool 已卸载完成。"
  ; Do not keep the installation directory open as our working directory.
  SetOutPath "$TEMP"
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function .onInstFailed
  !insertmacro FinishTransaction
FunctionEnd
Function un.onUninstFailed
  !insertmacro FinishTransaction
FunctionEnd

Function un.RemoveInstallerRegistration
  ClearErrors
  SetRegView 64
  ReadRegStr $0 HKLM "Software\PcToolInstaller" "InstallDir"
  ${If} $0 == "$INSTDIR"
    ClearErrors
    SetRegView 32
    DeleteRegKey HKLM "Software\PcToolInstaller"
    SetRegView 64
    DeleteRegKey HKLM "Software\PcToolInstaller"
    ${If} ${Errors}
      !insertmacro FinishTransaction
    SetErrorLevel 1
      MessageBox MB_ICONSTOP "安装恢复信息清理失败，已保留卸载器，请查看详细记录后重试。" /SD IDOK
      Abort
    ${EndIf}
  ${EndIf}
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool" "InstallLocation"
  ${If} $0 == "$INSTDIR"
    ClearErrors
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\PcTool"
    ${If} ${Errors}
      !insertmacro FinishTransaction
    SetErrorLevel 1
      MessageBox MB_ICONSTOP "卸载注册清理失败，已保留卸载器，请查看详细记录后重试。" /SD IDOK
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "Uninstall"
  !insertmacro PrepareTransaction
  ; A copied uninstaller can be run again after cleanup has finished. Do not
  ; create a new uninstall marker in a directory containing only user files.
  IfFileExists "$INSTDIR\PcTool.exe" uninstall_stop
  IfFileExists "$INSTDIR\.pctool-managed-files.json" uninstall_stop
  IfFileExists "$INSTDIR\.pctool-install-instance.json" uninstall_stop uninstall_unregister
  uninstall_stop:
  nsExec::ExecToLog '"$PLUGINSDIR\PcToolUninstallHelper.exe" mark "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "无法设置卸载状态，卸载未开始。" /SD IDOK
    Abort
  ${EndIf}
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\lifecycle.ps1" -Action RemoveStartup -Root "$INSTDIR" -Namespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "自启清理失败，已保留卸载重试入口。" /SD IDOK
    Abort
  ${EndIf}
  DetailPrint "正在停止本次安装的 PcTool 和内置 7-Zip 任务……"
  !insertmacro StopOwnedTasks
  uninstall_unregister:
  ; Remove only values still owned by this installation, before deleting files.
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\shell-integration.ps1" -Action Unregister -Root "$INSTDIR" -Namespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  ${If} $0 != 0
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "7-Zip 菜单撤销失败，已保留卸载器和恢复信息。请查看详细记录后重试。" /SD IDOK
    Abort
  ${EndIf}
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  IfFileExists "$INSTDIR\PcTool.exe" uninstall_files
  IfFileExists "$INSTDIR\.pctool-managed-files.json" uninstall_files
  IfFileExists "$INSTDIR\.pctool-install-instance.json" uninstall_files uninstall_registry
  uninstall_files:
  !insertmacro StopOwnedTasks
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-data.ps1" -Action Uninstall -Root "$INSTDIR" -CleanupNamespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $0
  ${If} $0 != 0
  ${AndIf} $0 != 2
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "部分受管文件清理失败，已保留卸载器和清理信息。请查看详细记录后重试卸载。" /SD IDOK
    Abort
  ${EndIf}
  Delete "$SMPROGRAMS\PcTool\PcTool.lnk"
  Delete "$DESKTOP\PcTool.lnk"
  Delete "$SMPROGRAMS\PcTool\压缩管理（7-Zip）.lnk"
  Delete "$SMPROGRAMS\PcTool\7-Zip 帮助.lnk"
  Delete "$SMPROGRAMS\PcTool\卸载 PcTool.lnk"
  RMDir "$SMPROGRAMS\PcTool"
  ; Report registry failures while the embedded retry uninstaller still exists.
  Call un.RemoveInstallerRegistration
  ; The marker is removed last by Finalize/the worker. Legacy loaded DLLs also
  ; keep refusing menus while any extension file is waiting for deletion.
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\manage-data.ps1" -Action Finalize -Root "$INSTDIR" -CleanupNamespace "${PCTOOL_CLEANUP_NAMESPACE}"'
  Pop $CleanupPending
  ${If} $CleanupPending != 0
  ${AndIf} $CleanupPending != 2
    !insertmacro FinishTransaction
    SetErrorLevel 1
    MessageBox MB_ICONSTOP "自动清理登记或文件删除失败，已保留重试入口。卸载未完成，请查看详细记录后重试。" /SD IDOK
    Abort
  ${EndIf}
  uninstall_registry:
  Call un.RemoveInstallerRegistration
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
  ${If} $CleanupPending == 2
    IfFileExists "$INSTDIR\.pctool-install-instance.json" pending_in_install pending_retired
    pending_retired:
    StrCpy $UninstallFinishText "PcTool 的菜单、自启及安装目录中的受管文件已清理。$\r$\n$\r$\nWindows 仍加载的扩展已迁至受保护的同盘清理区，占用释放后自动删除，无需重启电脑或资源管理器。"
    Goto pending_description_done
    pending_in_install:
    StrCpy $UninstallFinishText "PcTool 已停止运行，菜单、自启及系统注册已移除。$\r$\n$\r$\n其他程序仍占用少量文件，安装目录暂时保留，将在占用释放后自动清理，无需重启电脑。"
    pending_description_done:
    DetailPrint "$UninstallFinishText"
  ${Else}
    DetailPrint "PcTool 受管文件与系统注册已清理；用户自行保存的无关文件予以保留。"
  ${EndIf}
  !insertmacro FinishTransaction
  SetErrorLevel 0
SectionEnd

!macro customInstall
  ; Desktop shortcut for LivePlay Server
  CreateShortcut "$DESKTOP\LivePlay Server.lnk" \
    "$INSTDIR\resources\server-bin\liveplay-server.exe" \
    "--port 4480" \
    "$INSTDIR\resources\server-bin\liveplay-server.exe" 0
!macroend

!macro customUnInstall
  Delete "$DESKTOP\LivePlay Server.lnk"
!macroend

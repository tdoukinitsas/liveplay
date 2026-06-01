!macro customInstall
  ; Desktop shortcut for LivePlay Server
  CreateShortcut "$DESKTOP\LivePlay Server.lnk" \
    "$INSTDIR\resources\server-bin\liveplay-server.exe" \
    "--port 4480" \
    "$INSTDIR\resources\server-bin\liveplay-server.exe" 0

  ; Firewall rules so LAN auto-discovery works out of the box. The installer
  ; runs elevated, so this is the authoritative place to add them (the apps
  ; also try at runtime as a fallback, but that needs admin). Discovery uses
  ; inbound UDP: the server receiving client solicitations, the client
  ; receiving beacons / unicast replies. Program-scoped rules cover all the
  ; ports we use (UDP/4481 discovery + TCP/4480 control). Delete any stale
  ; rules first so re-installs don't pile up duplicates.
  nsExec::Exec 'netsh advfirewall firewall delete rule name="LivePlay Server (UDP-In)"'
  nsExec::Exec 'netsh advfirewall firewall delete rule name="LivePlay Server (TCP-In)"'
  nsExec::Exec 'netsh advfirewall firewall delete rule name="LivePlay Client (UDP-In)"'
  nsExec::Exec 'netsh advfirewall firewall add rule name="LivePlay Server (UDP-In)" dir=in action=allow protocol=UDP program="$INSTDIR\resources\server-bin\liveplay-server.exe" enable=yes profile=any'
  nsExec::Exec 'netsh advfirewall firewall add rule name="LivePlay Server (TCP-In)" dir=in action=allow protocol=TCP program="$INSTDIR\resources\server-bin\liveplay-server.exe" enable=yes profile=any'
  nsExec::Exec 'netsh advfirewall firewall add rule name="LivePlay Client (UDP-In)" dir=in action=allow protocol=UDP program="$INSTDIR\${APP_EXECUTABLE_FILENAME}" enable=yes profile=any'
!macroend

!macro customUnInstall
  Delete "$DESKTOP\LivePlay Server.lnk"

  ; Remove the firewall rules we added during install.
  nsExec::Exec 'netsh advfirewall firewall delete rule name="LivePlay Server (UDP-In)"'
  nsExec::Exec 'netsh advfirewall firewall delete rule name="LivePlay Server (TCP-In)"'
  nsExec::Exec 'netsh advfirewall firewall delete rule name="LivePlay Client (UDP-In)"'
!macroend

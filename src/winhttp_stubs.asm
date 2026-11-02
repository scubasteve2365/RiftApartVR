EXTERN g_realWinhttp:QWORD
.code
DllCanUnloadNow_stub PROC
  jmp QWORD PTR [g_realWinhttp + 0]
DllCanUnloadNow_stub ENDP
DllGetClassObject_stub PROC
  jmp QWORD PTR [g_realWinhttp + 8]
DllGetClassObject_stub ENDP
Private1_stub PROC
  jmp QWORD PTR [g_realWinhttp + 16]
Private1_stub ENDP
SvchostPushServiceGlobals_stub PROC
  jmp QWORD PTR [g_realWinhttp + 24]
SvchostPushServiceGlobals_stub ENDP
WinHttpAddRequestHeaders_stub PROC
  jmp QWORD PTR [g_realWinhttp + 32]
WinHttpAddRequestHeaders_stub ENDP
WinHttpAddRequestHeadersEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 40]
WinHttpAddRequestHeadersEx_stub ENDP
WinHttpAutoProxySvcMain_stub PROC
  jmp QWORD PTR [g_realWinhttp + 48]
WinHttpAutoProxySvcMain_stub ENDP
WinHttpCheckPlatform_stub PROC
  jmp QWORD PTR [g_realWinhttp + 56]
WinHttpCheckPlatform_stub ENDP
WinHttpCloseHandle_stub PROC
  jmp QWORD PTR [g_realWinhttp + 64]
WinHttpCloseHandle_stub ENDP
WinHttpConnect_stub PROC
  jmp QWORD PTR [g_realWinhttp + 72]
WinHttpConnect_stub ENDP
WinHttpConnectionDeletePolicyEntries_stub PROC
  jmp QWORD PTR [g_realWinhttp + 80]
WinHttpConnectionDeletePolicyEntries_stub ENDP
WinHttpConnectionDeletePolicyEntriesByAppSid_stub PROC
  jmp QWORD PTR [g_realWinhttp + 88]
WinHttpConnectionDeletePolicyEntriesByAppSid_stub ENDP
WinHttpConnectionDeleteProxyInfo_stub PROC
  jmp QWORD PTR [g_realWinhttp + 96]
WinHttpConnectionDeleteProxyInfo_stub ENDP
WinHttpConnectionFreeNameList_stub PROC
  jmp QWORD PTR [g_realWinhttp + 104]
WinHttpConnectionFreeNameList_stub ENDP
WinHttpConnectionFreeProxyInfo_stub PROC
  jmp QWORD PTR [g_realWinhttp + 112]
WinHttpConnectionFreeProxyInfo_stub ENDP
WinHttpConnectionFreeProxyList_stub PROC
  jmp QWORD PTR [g_realWinhttp + 120]
WinHttpConnectionFreeProxyList_stub ENDP
WinHttpConnectionGetNameList_stub PROC
  jmp QWORD PTR [g_realWinhttp + 128]
WinHttpConnectionGetNameList_stub ENDP
WinHttpConnectionGetProxyInfo_stub PROC
  jmp QWORD PTR [g_realWinhttp + 136]
WinHttpConnectionGetProxyInfo_stub ENDP
WinHttpConnectionGetProxyList_stub PROC
  jmp QWORD PTR [g_realWinhttp + 144]
WinHttpConnectionGetProxyList_stub ENDP
WinHttpConnectionOnlyConvert_stub PROC
  jmp QWORD PTR [g_realWinhttp + 152]
WinHttpConnectionOnlyConvert_stub ENDP
WinHttpConnectionOnlyReceive_stub PROC
  jmp QWORD PTR [g_realWinhttp + 160]
WinHttpConnectionOnlyReceive_stub ENDP
WinHttpConnectionOnlySend_stub PROC
  jmp QWORD PTR [g_realWinhttp + 168]
WinHttpConnectionOnlySend_stub ENDP
WinHttpConnectionSetPolicyEntries_stub PROC
  jmp QWORD PTR [g_realWinhttp + 176]
WinHttpConnectionSetPolicyEntries_stub ENDP
WinHttpConnectionSetProxyInfo_stub PROC
  jmp QWORD PTR [g_realWinhttp + 184]
WinHttpConnectionSetProxyInfo_stub ENDP
WinHttpConnectionUpdateIfIndexTable_stub PROC
  jmp QWORD PTR [g_realWinhttp + 192]
WinHttpConnectionUpdateIfIndexTable_stub ENDP
WinHttpCrackUrl_stub PROC
  jmp QWORD PTR [g_realWinhttp + 200]
WinHttpCrackUrl_stub ENDP
WinHttpCreateProxyList_stub PROC
  jmp QWORD PTR [g_realWinhttp + 208]
WinHttpCreateProxyList_stub ENDP
WinHttpCreateProxyManager_stub PROC
  jmp QWORD PTR [g_realWinhttp + 216]
WinHttpCreateProxyManager_stub ENDP
WinHttpCreateProxyResolver_stub PROC
  jmp QWORD PTR [g_realWinhttp + 224]
WinHttpCreateProxyResolver_stub ENDP
WinHttpCreateProxyResult_stub PROC
  jmp QWORD PTR [g_realWinhttp + 232]
WinHttpCreateProxyResult_stub ENDP
WinHttpCreateUiCompatibleProxyString_stub PROC
  jmp QWORD PTR [g_realWinhttp + 240]
WinHttpCreateUiCompatibleProxyString_stub ENDP
WinHttpCreateUrl_stub PROC
  jmp QWORD PTR [g_realWinhttp + 248]
WinHttpCreateUrl_stub ENDP
WinHttpDetectAutoProxyConfigUrl_stub PROC
  jmp QWORD PTR [g_realWinhttp + 256]
WinHttpDetectAutoProxyConfigUrl_stub ENDP
WinHttpFreeProxyResult_stub PROC
  jmp QWORD PTR [g_realWinhttp + 264]
WinHttpFreeProxyResult_stub ENDP
WinHttpFreeProxyResultEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 272]
WinHttpFreeProxyResultEx_stub ENDP
WinHttpFreeProxySettings_stub PROC
  jmp QWORD PTR [g_realWinhttp + 280]
WinHttpFreeProxySettings_stub ENDP
WinHttpFreeProxySettingsEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 288]
WinHttpFreeProxySettingsEx_stub ENDP
WinHttpFreeQueryConnectionGroupResult_stub PROC
  jmp QWORD PTR [g_realWinhttp + 296]
WinHttpFreeQueryConnectionGroupResult_stub ENDP
WinHttpGetDefaultProxyConfiguration_stub PROC
  jmp QWORD PTR [g_realWinhttp + 304]
WinHttpGetDefaultProxyConfiguration_stub ENDP
WinHttpGetIEProxyConfigForCurrentUser_stub PROC
  jmp QWORD PTR [g_realWinhttp + 312]
WinHttpGetIEProxyConfigForCurrentUser_stub ENDP
WinHttpGetProxyForUrl_stub PROC
  jmp QWORD PTR [g_realWinhttp + 320]
WinHttpGetProxyForUrl_stub ENDP
WinHttpGetProxyForUrlEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 328]
WinHttpGetProxyForUrlEx_stub ENDP
WinHttpGetProxyForUrlEx2_stub PROC
  jmp QWORD PTR [g_realWinhttp + 336]
WinHttpGetProxyForUrlEx2_stub ENDP
WinHttpGetProxyForUrlHvsi_stub PROC
  jmp QWORD PTR [g_realWinhttp + 344]
WinHttpGetProxyForUrlHvsi_stub ENDP
WinHttpGetProxyResult_stub PROC
  jmp QWORD PTR [g_realWinhttp + 352]
WinHttpGetProxyResult_stub ENDP
WinHttpGetProxyResultEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 360]
WinHttpGetProxyResultEx_stub ENDP
WinHttpGetProxySettingsEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 368]
WinHttpGetProxySettingsEx_stub ENDP
WinHttpGetProxySettingsResultEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 376]
WinHttpGetProxySettingsResultEx_stub ENDP
WinHttpGetProxySettingsVersion_stub PROC
  jmp QWORD PTR [g_realWinhttp + 384]
WinHttpGetProxySettingsVersion_stub ENDP
WinHttpGetTunnelSocket_stub PROC
  jmp QWORD PTR [g_realWinhttp + 392]
WinHttpGetTunnelSocket_stub ENDP
WinHttpOpen_stub PROC
  jmp QWORD PTR [g_realWinhttp + 400]
WinHttpOpen_stub ENDP
WinHttpOpenRequest_stub PROC
  jmp QWORD PTR [g_realWinhttp + 408]
WinHttpOpenRequest_stub ENDP
WinHttpPacJsWorkerMain_stub PROC
  jmp QWORD PTR [g_realWinhttp + 416]
WinHttpPacJsWorkerMain_stub ENDP
WinHttpProbeConnectivity_stub PROC
  jmp QWORD PTR [g_realWinhttp + 424]
WinHttpProbeConnectivity_stub ENDP
WinHttpProtocolCompleteUpgrade_stub PROC
  jmp QWORD PTR [g_realWinhttp + 432]
WinHttpProtocolCompleteUpgrade_stub ENDP
WinHttpProtocolReceive_stub PROC
  jmp QWORD PTR [g_realWinhttp + 440]
WinHttpProtocolReceive_stub ENDP
WinHttpProtocolSend_stub PROC
  jmp QWORD PTR [g_realWinhttp + 448]
WinHttpProtocolSend_stub ENDP
WinHttpQueryAuthSchemes_stub PROC
  jmp QWORD PTR [g_realWinhttp + 456]
WinHttpQueryAuthSchemes_stub ENDP
WinHttpQueryConnectionGroup_stub PROC
  jmp QWORD PTR [g_realWinhttp + 464]
WinHttpQueryConnectionGroup_stub ENDP
WinHttpQueryDataAvailable_stub PROC
  jmp QWORD PTR [g_realWinhttp + 472]
WinHttpQueryDataAvailable_stub ENDP
WinHttpQueryHeaders_stub PROC
  jmp QWORD PTR [g_realWinhttp + 480]
WinHttpQueryHeaders_stub ENDP
WinHttpQueryHeadersEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 488]
WinHttpQueryHeadersEx_stub ENDP
WinHttpQueryOption_stub PROC
  jmp QWORD PTR [g_realWinhttp + 496]
WinHttpQueryOption_stub ENDP
WinHttpReadData_stub PROC
  jmp QWORD PTR [g_realWinhttp + 504]
WinHttpReadData_stub ENDP
WinHttpReadDataEx_stub PROC
  jmp QWORD PTR [g_realWinhttp + 512]
WinHttpReadDataEx_stub ENDP
WinHttpReadProxySettings_stub PROC
  jmp QWORD PTR [g_realWinhttp + 520]
WinHttpReadProxySettings_stub ENDP
WinHttpReadProxySettingsHvsi_stub PROC
  jmp QWORD PTR [g_realWinhttp + 528]
WinHttpReadProxySettingsHvsi_stub ENDP
WinHttpReceiveResponse_stub PROC
  jmp QWORD PTR [g_realWinhttp + 536]
WinHttpReceiveResponse_stub ENDP
WinHttpRefreshProxySettings_stub PROC
  jmp QWORD PTR [g_realWinhttp + 544]
WinHttpRefreshProxySettings_stub ENDP
WinHttpRegisterProxyChangeNotification_stub PROC
  jmp QWORD PTR [g_realWinhttp + 552]
WinHttpRegisterProxyChangeNotification_stub ENDP
WinHttpResetAutoProxy_stub PROC
  jmp QWORD PTR [g_realWinhttp + 560]
WinHttpResetAutoProxy_stub ENDP
WinHttpResolverGetProxyForUrl_stub PROC
  jmp QWORD PTR [g_realWinhttp + 568]
WinHttpResolverGetProxyForUrl_stub ENDP
WinHttpSaveProxyCredentials_stub PROC
  jmp QWORD PTR [g_realWinhttp + 576]
WinHttpSaveProxyCredentials_stub ENDP
WinHttpSendRequest_stub PROC
  jmp QWORD PTR [g_realWinhttp + 584]
WinHttpSendRequest_stub ENDP
WinHttpSetCredentials_stub PROC
  jmp QWORD PTR [g_realWinhttp + 592]
WinHttpSetCredentials_stub ENDP
WinHttpSetDefaultProxyConfiguration_stub PROC
  jmp QWORD PTR [g_realWinhttp + 600]
WinHttpSetDefaultProxyConfiguration_stub ENDP
WinHttpSetOption_stub PROC
  jmp QWORD PTR [g_realWinhttp + 608]
WinHttpSetOption_stub ENDP
WinHttpSetProxySettingsPerUser_stub PROC
  jmp QWORD PTR [g_realWinhttp + 616]
WinHttpSetProxySettingsPerUser_stub ENDP
WinHttpSetSecureLegacyServersAppCompat_stub PROC
  jmp QWORD PTR [g_realWinhttp + 624]
WinHttpSetSecureLegacyServersAppCompat_stub ENDP
WinHttpSetStatusCallback_stub PROC
  jmp QWORD PTR [g_realWinhttp + 632]
WinHttpSetStatusCallback_stub ENDP
WinHttpSetTimeouts_stub PROC
  jmp QWORD PTR [g_realWinhttp + 640]
WinHttpSetTimeouts_stub ENDP
WinHttpTimeFromSystemTime_stub PROC
  jmp QWORD PTR [g_realWinhttp + 648]
WinHttpTimeFromSystemTime_stub ENDP
WinHttpTimeToSystemTime_stub PROC
  jmp QWORD PTR [g_realWinhttp + 656]
WinHttpTimeToSystemTime_stub ENDP
WinHttpUnregisterProxyChangeNotification_stub PROC
  jmp QWORD PTR [g_realWinhttp + 664]
WinHttpUnregisterProxyChangeNotification_stub ENDP
WinHttpWebSocketClose_stub PROC
  jmp QWORD PTR [g_realWinhttp + 672]
WinHttpWebSocketClose_stub ENDP
WinHttpWebSocketCompleteUpgrade_stub PROC
  jmp QWORD PTR [g_realWinhttp + 680]
WinHttpWebSocketCompleteUpgrade_stub ENDP
WinHttpWebSocketQueryCloseStatus_stub PROC
  jmp QWORD PTR [g_realWinhttp + 688]
WinHttpWebSocketQueryCloseStatus_stub ENDP
WinHttpWebSocketReceive_stub PROC
  jmp QWORD PTR [g_realWinhttp + 696]
WinHttpWebSocketReceive_stub ENDP
WinHttpWebSocketSend_stub PROC
  jmp QWORD PTR [g_realWinhttp + 704]
WinHttpWebSocketSend_stub ENDP
WinHttpWebSocketShutdown_stub PROC
  jmp QWORD PTR [g_realWinhttp + 712]
WinHttpWebSocketShutdown_stub ENDP
WinHttpWriteData_stub PROC
  jmp QWORD PTR [g_realWinhttp + 720]
WinHttpWriteData_stub ENDP
WinHttpWriteProxySettings_stub PROC
  jmp QWORD PTR [g_realWinhttp + 728]
WinHttpWriteProxySettings_stub ENDP
END

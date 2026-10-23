# 管理者PowerShellで実行
sc.exe stop   ApexRing0
sc.exe delete ApexRing0
bcdedit /set testsigning off
Remove-Item "$PSScriptRoot\apex_test.cer","$PSScriptRoot\apex_test.pfx" -ErrorAction SilentlyContinue
Write-Host "アンインストール完了。再起動してください。" -ForegroundColor Green
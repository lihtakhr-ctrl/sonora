# 管理者PowerShellで実行
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sys  = Get-ChildItem -Path $root -Recurse -Filter "ApexRing0.sys" |
        Where-Object { $_.FullName -match "x64" } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $sys) { throw "ApexRing0.sys が見つかりません。先にドライバをビルドしてください。" }

$cert = Join-Path $root "apex_test.cer"
$pfx  = Join-Path $root "apex_test.pfx"
$pass = ConvertTo-SecureString "venice" -AsPlainText -Force

Write-Host "[1/5] 証明書を作成" -ForegroundColor Cyan
$c = New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject "CN=Venice Apex Test" -KeyUsage DigitalSignature -KeyLength 2048 `
        -CertStoreLocation "Cert:\CurrentUser\My" -NotAfter (Get-Date).AddYears(10)
Export-PfxCertificate -Cert $c -FilePath $pfx -Password $pass | Out-Null
Export-Certificate  -Cert $c -FilePath $cert | Out-Null

Write-Host "[2/5] 信頼ストアへ登録" -ForegroundColor Cyan
Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

Write-Host "[3/5] 署名" -ForegroundColor Cyan
$signtool = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" |
             Sort-Object FullName -Descending | Select-Object -First 1).FullName
if (-not $signtool) { throw "signtool.exe が見つかりません。Windows SDK を入れてください。" }
& $signtool sign /fd SHA256 /f $pfx /p venice /v $sys.FullName

Write-Host "[4/5] テスト署名を有効化" -ForegroundColor Cyan
bcdedit /set testsigning on

Write-Host "[5/5] サービス登録・起動" -ForegroundColor Cyan
sc.exe stop   ApexRing0 2>$null
sc.exe delete ApexRing0 2>$null
sc.exe create ApexRing0 type= kernel binPath= $sys.FullName start= demand
sc.exe start  ApexRing0

Write-Host "`n完了しました。" -ForegroundColor Green
Write-Host "Secure Boot を OFF にした場合は再起動してください。" -ForegroundColor Yellow
Write-Host "スプーファは揮発性です。Apex 起動前にドライバをロードしてください。" -ForegroundColor Yellow
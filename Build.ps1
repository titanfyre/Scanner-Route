param([switch]$Asan,[string]$SdkDirectory)
$ErrorActionPreference='Stop'
$directory=Join-Path $PSScriptRoot $(if($Asan){'obj\Asan'}else{'obj\Build'})
$arguments=@('-S',$PSScriptRoot,'-B',$directory,'-G','Visual Studio 17 2022','-A','x64')
if($Asan){$arguments+='-DSCANNER_ASAN=ON'}
if($SdkDirectory){$arguments+=('-DSDK_DIRECTORY='+$SdkDirectory)}
& cmake @arguments
if($LASTEXITCODE){throw 'Configuration failed.'}
$targets=@('--build',$directory,'--config','Release')
if($Asan){$targets+=@('--target','ScannerTests')}
& cmake @targets
if($LASTEXITCODE){throw 'Compilation failed.'}
& ctest --test-dir $directory -C Release --output-on-failure
if($LASTEXITCODE){throw 'Offline tests failed.'}
if(!$Asan){
    $process=Start-Process -FilePath (Join-Path $directory 'Release\ScannerRoute.exe') -ArgumentList '--ui-self-test' -WindowStyle Hidden -Wait -PassThru
    if($process.ExitCode){throw 'Offline GUI self-test failed.'}
}

param (
 $btype="debug"
)

Set-StrictMode -Version Latest

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$includes = " "
$includes += "-I " + (rvpa 'Libs/imgui').path + " "
$includes += "-I " + (rvpa 'Libs/imgui/backends').path + " "
$includes += "-I " + (rvpa 'Libs/imgui/misc').path + " "
$includes += "-I " + (rvpa 'Libs/sqlite').path + " "

$options = " "
$options += "-std=c++20"

$optimize = " "

if ("debug" -eq $btype) {
	$optimize += "-g -Og"
}
if ("release" -eq $btype) {
	$optimize += "-O3"
}


$links = " "
$links += "-L 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64'"
$links += " -ld3d12 -ld3dcompiler -ldxgi -lgdi32 -ldwmapi"

if ("clean" -eq $btype) {
	if (test-path "build-win64") {
		rm 'build-win64' -recurse -force
	}
	write-host "Done clean"
	exit 0
}

$buildloc = (join-path 'build-win64' $btype)

ni -itemtype 'directory' $buildloc -force > $null

$buildloc = (rvpa $buildloc).path

$global:built = 0
$global:exe = "main.exe"

function build_files {
	param (
		[Parameter(Mandatory=$true, Position=0)]
		[string] $pattern,
		[Parameter(Mandatory=$true, Position=1)]
		[string] $folder,
		[Parameter(Position=2)]
		[string] $comp="g++",
		[Parameter(Position=3)]
		[string] $opt=$options
	)
	
	push-location $folder
	
	trap { pop-location }
	
	gci $pattern | % {
		$outfile = (join-path $buildloc ($_.basename + ".o"))
		
		if (-not (test-path $outfile) -or ((gci $outfile).LastWriteTime -lt $_.LastWriteTime)) {
			$line = $comp + " -c -o " + $outfile + " " + $_.fullname + $includes + $opt + $optimize
			write-output $line
			iex $line
			write-host ("result: [" + $LASTEXITCODE + "]")
			if (0 -ne $LASTEXITCODE) {
				pop-location
				write-host "Had Error " $LASTEXITCODE " " $line
				exit 1
			}
			
			$global:built = $global:built + 1
		} else {
			("Already built " + $outfile)
			if ((gci $outfile).LastWriteTime -gt (gci (join-path $buildloc $global:exe)).LastWriteTime) {
				$global:built += 1
			}
		}
	}
	
	pop-location
}

build_files "imgui_impl_dx12.cpp" "./Libs/imgui/backends"
build_files "imgui_impl_win32.cpp" "./Libs/imgui/backends"
build_files "imgui_stdlib.cpp" "./Libs/imgui/misc/cpp"
build_files "imgui*.cpp" "./Libs/imgui"
build_files "sqlite3.c" "./Libs/sqlite" "gcc" ""
build_files "*.cpp" "./"

if ($global:built -ne 0) {
	push-location $buildloc

	$list = (gci *.o | %{$_.name})
	$cmd = "&g++ -o " + $global:exe + " " + [string]::Join(" ",$list) + $includes + $options + $links + " " + $optimize
	write-host "exec: " $cmd
	iex $cmd

	pop-location
}

if (0 -ne $LASTEXITCODE) {
	write-host "failed: " $cmd
	write-host "Had Error " $LASTEXITCODE " " $cmd
	exit 1
}

write-host "Binary " $buildloc

write-host "Done"


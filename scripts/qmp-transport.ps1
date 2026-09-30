# Private filesystem sockets keep QMP outside the guest's network access.
if (-not ('OmarchyUnixEndPoint' -as [type])) {
Add-Type @'
using System;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public class OmarchyUnixEndPoint : EndPoint {
 readonly string path;
 public OmarchyUnixEndPoint(string value){path=value;}
 public override AddressFamily AddressFamily { get {return AddressFamily.Unix;} }
 public override SocketAddress Serialize(){
  byte[] bytes=Encoding.UTF8.GetBytes(path);
  if(bytes.Length>103)throw new ArgumentException("Private control path is too long");
  // Winsock's async connection path expects the complete sockaddr_un buffer.
  var address=new SocketAddress(AddressFamily.Unix,110);
  for(int i=0;i<bytes.Length;i++)address[i+2]=bytes[i];
  return address;
 }
 public override EndPoint Create(SocketAddress address){return this;}
 [StructLayout(LayoutKind.Sequential)] struct TagInfo {public uint Attributes;public uint Tag;}
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFile(string path,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileInformationByHandleEx(SafeFileHandle file,int kind,out TagInfo info,uint size);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern uint GetShortPathName(string path,StringBuilder result,uint size);
 public static bool IsSocket(string path){
  using(var file=CreateFile(path,0,7,IntPtr.Zero,3,0x02200000,IntPtr.Zero)){
   TagInfo info;
   return !file.IsInvalid && GetFileInformationByHandleEx(file,9,out info,8) && info.Tag==0x80000023;
  }
 }
 public static string ShortPath(string path){
  var result=new StringBuilder(32768);
  uint size=GetShortPathName(path,result,(uint)result.Capacity);
  return size>0 && size<result.Capacity ? result.ToString() : path;
 }
 public static Socket Connect(string path,int timeout){
  var socket=new Socket(AddressFamily.Unix,SocketType.Stream,ProtocolType.Unspecified);
  try {
   var pending=socket.BeginConnect(new OmarchyUnixEndPoint(path),null,null);
   // Completed async results can share a wait handle on modern .NET. Do not
   // dispose that shared handle between successive private connections.
   var clock=System.Diagnostics.Stopwatch.StartNew();
   while(!pending.IsCompleted){
    if(clock.ElapsedMilliseconds>=timeout)throw new TimeoutException("Private control connection timed out");
    System.Threading.Thread.Sleep(10);
   }
   socket.EndConnect(pending);
   return socket;
  }catch{socket.Close();throw;}
 }
}
'@
}
function Get-OmarchyQmpPath([int]$Role=4445) {
 $name=switch($Role){4445 {'tools.sock'} 4446 {'forward.sock'} 4447 {'supervisor.sock'} default {throw 'Unknown QMP control role'}}
 $cache=[IO.Path]::GetTempPath()
 if([string]::IsNullOrWhiteSpace($cache)){throw 'Windows temporary directory is unavailable'}
 if([Text.Encoding]::UTF8.GetByteCount((Join-Path $cache 'TryOmarchyIPC\supervisor.sock')) -gt 103){$cache=[OmarchyUnixEndPoint]::ShortPath($cache)}
 $path=Join-Path $cache "TryOmarchyIPC\$name"
 if([Text.Encoding]::UTF8.GetByteCount($path) -gt 103){throw 'Private control path is too long'}
 return $path
}
function New-OmarchyQmpStream([int]$Role=4445) {
 $socket=[OmarchyUnixEndPoint]::Connect((Get-OmarchyQmpPath $Role),3000)
 return New-Object Net.Sockets.NetworkStream($socket,$true)
}
function Initialize-OmarchyQmpControl {
 $paths=@(4445,4446,4447 | ForEach-Object {Get-OmarchyQmpPath $_})
 $directory=Split-Path $paths[0]
 for($cursor=$directory;$cursor;$cursor=[IO.Path]::GetDirectoryName($cursor)){
  if((Test-Path -LiteralPath $cursor) -and ([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint)){throw "Linked private control directory: $cursor"}
 }
 [IO.Directory]::CreateDirectory($directory) | Out-Null
 $stale=@()
 foreach($path in $paths){
  if(-not (Test-Path -LiteralPath $path)){continue}
  if(-not [OmarchyUnixEndPoint]::IsSocket($path)){throw "Private control path contains another file: $path"}
  $socket=$null
  try {$socket=[OmarchyUnixEndPoint]::Connect($path,1000)}
  catch {
   $cause=$_.Exception
   while($cause.InnerException){$cause=$cause.InnerException}
   if($cause -isnot [Net.Sockets.SocketException] -or $cause.NativeErrorCode -ne 10061){throw}
  }
  if($socket){$socket.Close();throw 'An Omarchy runtime still owns its private control socket'}
  $stale+=$path
 }
 foreach($path in $stale){Remove-Item -LiteralPath $path -Force}
}

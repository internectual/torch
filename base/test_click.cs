function zclick(){
  %t = 0;
  while(%t < $HostTypeCount){
    if($HostTypeName[%t] $= "ctf"){
      echo("CLICK CTF IDX=" @ %t);
      GMH_MissionType.onSelect(%t, "");
      echo("LIST SIZE=" @ GMH_MissionList.size());
      %m = 0;
      while(%m < GMH_MissionList.size()){
        echo("MAP["@ %m @ "]=" @ GMH_MissionList.getRowTextById(%m));
        %m = %m + 1;
      }
      break;
    }
    %t = %t + 1;
  }
}
zclick();

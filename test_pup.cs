function zpop(){
  echo("PTYPE COUNT=" @ GMH_MissionType.size());
  %t = 0;
  while(%t < GMH_MissionType.size()){
    echo("PTYPE["@ %t @ "]=" @ GMH_MissionType[%t]);
    %t = %t + 1;
  }
  %APPDA selected=%GMH_MissionType.getSelectedId();
  echo("SELECTED ID=" @ selected);
}
zpop();

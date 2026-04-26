// Astro Toolkit — combined Qt6 application
// Tabs: Target Picker (original cpp_pr) + FITS Reviewer + Tools (Sort/Stack)

#include <QtWidgets>
#include <QtConcurrent>
#include <QtSql>
#include <QScreen>
#include <QStandardPaths>
#include <QProcess>
#include <QFileDialog>
#include <QSplitter>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>
#include <QPainter>
#include <QKeyEvent>
#include <QFutureWatcher>
#include <QScrollArea>

#include <fitsio.h>

#include <cmath>
#include <optional>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <ctime>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef HAVE_GPSD
  #include <gps.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Small math
// ============================================================
static double deg2rad(double d) { return d * M_PI / 180.0; }
static double rad2deg(double r) { return r * 180.0 / M_PI; }
static double wrap2pi(double x) {
  x = std::fmod(x, 2.0 * M_PI);
  if (x < 0) x += 2.0 * M_PI;
  return x;
}

// ============================================================
// Config
// ============================================================
static QString appDirPath() {
#ifdef Q_OS_WIN
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QDir::separator() + "astro_gui";
#else
  return QDir(QDir::homePath() + "/.astro_gui").absolutePath();
#endif
}
static QString configPath() { return QDir(appDirPath()).filePath("config.json"); }

// ============================================================
// Gear Profiles
// ============================================================
struct GearProfile {
  QString name       = "My Setup";
  QString scopeType  = "Newtonian";
  double  focalLenMm = 1000.0;
  double  apertureMm = 200.0;
  QString cameraName = "";
  double  pixelUm    = 3.76;
  int     widthPx    = 3008;
  int     heightPx   = 3008;
  bool    hasLRGB    = true;
  bool    hasHa      = false;
  bool    hasOIII    = false;
  bool    hasSII     = false;
};
static const QStringList kScopeTypes{"Newtonian","Refractor","SCT","RC","Mak-Cas","Mak-New","Dobsonian"};
static std::vector<GearProfile> builtinPresets(){
  std::vector<GearProfile> v;
  {GearProfile p;p.name="Newton 200/1000 + ToupTek ATR533MM";p.focalLenMm=1000;p.apertureMm=200;p.cameraName="ToupTek ATR533MM";p.pixelUm=3.76;p.widthPx=3008;p.heightPx=3008;p.hasHa=p.hasOIII=p.hasSII=true;v.push_back(p);}
  {GearProfile p;p.name="Newton 200/1000 + ZWO ASI2600MM";p.focalLenMm=1000;p.apertureMm=200;p.cameraName="ZWO ASI2600MM";p.pixelUm=3.76;p.widthPx=6248;p.heightPx=4176;p.hasHa=p.hasOIII=p.hasSII=true;v.push_back(p);}
  {GearProfile p;p.name="Refractor 80/480 + ZWO ASI294MM";p.scopeType="Refractor";p.focalLenMm=480;p.apertureMm=80;p.cameraName="ZWO ASI294MM";p.pixelUm=4.63;p.widthPx=4144;p.heightPx=2822;p.hasHa=p.hasOIII=true;v.push_back(p);}
  {GearProfile p;p.name="SCT 8\" 2032mm + ZWO ASI183MM";p.scopeType="SCT";p.focalLenMm=2032;p.apertureMm=203;p.cameraName="ZWO ASI183MM";p.pixelUm=2.4;p.widthPx=5496;p.heightPx=3672;p.hasLRGB=true;p.hasHa=true;v.push_back(p);}
  return v;
}
static json profileToJson(const GearProfile& p){
  json j;
  j["name"]=p.name.toStdString();j["scope_type"]=p.scopeType.toStdString();
  j["focal_length_mm"]=p.focalLenMm;j["aperture_mm"]=p.apertureMm;
  j["camera_name"]=p.cameraName.toStdString();
  j["pixel_um"]=p.pixelUm;j["width_px"]=p.widthPx;j["height_px"]=p.heightPx;
  j["has_lrgb"]=p.hasLRGB;j["has_ha"]=p.hasHa;j["has_oiii"]=p.hasOIII;j["has_sii"]=p.hasSII;
  return j;
}
static GearProfile profileFromJson(const json& j){
  GearProfile p;
  auto s=[&](const char*k,const QString& d)->QString{return j.contains(k)&&j[k].is_string()?QString::fromStdString(j[k].get<std::string>()):d;};
  auto d=[&](const char*k,double v)->double{return j.contains(k)&&j[k].is_number()?j[k].get<double>():v;};
  auto ii=[&](const char*k,int v)->int{return j.contains(k)&&j[k].is_number_integer()?j[k].get<int>():v;};
  auto b=[&](const char*k,bool v)->bool{return j.contains(k)&&j[k].is_boolean()?j[k].get<bool>():v;};
  p.name=s("name","My Setup");p.scopeType=s("scope_type","Newtonian");
  p.focalLenMm=d("focal_length_mm",1000);p.apertureMm=d("aperture_mm",200);
  p.cameraName=s("camera_name","");
  p.pixelUm=d("pixel_um",3.76);p.widthPx=ii("width_px",3008);p.heightPx=ii("height_px",3008);
  p.hasLRGB=b("has_lrgb",true);p.hasHa=b("has_ha",false);p.hasOIII=b("has_oiii",false);p.hasSII=b("has_sii",false);
  return p;
}

struct AppConfig {
  QString sqlitePath = appDirPath() + QDir::separator() + "catalog.sqlite";
  QString openaiKey;
  QString openaiModel = "gpt-4o-mini";
  QString ipGeoUrl = "https://ipapi.co/json/";
  double minAltDeg = 30.0;
  double minHoursAbove = 4.0;
  int pageSize = 5;
  QVector<GearProfile> gearProfiles;
  int activeProfile = 0;
  // Derived from active profile — call syncFromActiveProfile() after load
  std::optional<double> focalLengthMm;
  std::optional<double> pixelSizeUm;
  int sensorWidthPx  = 3008;
  int sensorHeightPx = 3008;
  void syncFromActiveProfile(){
    if(activeProfile>=0&&activeProfile<gearProfiles.size()){
      const auto& p=gearProfiles[activeProfile];
      focalLengthMm=p.focalLenMm;pixelSizeUm=p.pixelUm;
      sensorWidthPx=p.widthPx;sensorHeightPx=p.heightPx;
    }else{focalLengthMm={};pixelSizeUm={};}
  }
  const GearProfile* activeGear()const{
    if(activeProfile>=0&&activeProfile<gearProfiles.size())return &gearProfiles[activeProfile];
    return nullptr;
  }
};

static void saveDefaultConfig() {
  QDir().mkpath(appDirPath());
  QFile f(configPath());
  if (f.exists()) return;
  if (!f.open(QIODevice::WriteOnly)) return;
  json cfg;
  cfg["sqlite_db_path"] = (appDirPath() + QDir::separator() + "catalog.sqlite").toStdString();
  cfg["openai_api_key"] = "";
  cfg["openai_model"] = "gpt-4o-mini";
  cfg["ip_geo_url"] = "https://ipapi.co/json/";
  cfg["min_alt_deg"] = 30.0;
  cfg["min_hours_above"] = 4.0;
  cfg["page_size"] = 5;
  cfg["focal_length_mm"] = 1000.0;
  cfg["pixel_size_um"] = 3.76;
  cfg["sensor_width_px"]  = 3008;
  cfg["sensor_height_px"] = 3008;
  std::string s = cfg.dump(2);
  f.write(s.c_str(), s.size());
}

static AppConfig loadConfig() {
  AppConfig cfg;
  QDir().mkpath(appDirPath());
  saveDefaultConfig();
  QFile f(configPath());
  if (!f.exists() || !f.open(QIODevice::ReadOnly)) return cfg;
  try {
    auto doc = json::parse(f.readAll().toStdString());
    if (doc.contains("sqlite_db_path")) cfg.sqlitePath = QString::fromStdString(doc["sqlite_db_path"].get<std::string>());
    if (doc.contains("openai_api_key")) { QString k = QString::fromStdString(doc["openai_api_key"].get<std::string>()); cfg.openaiKey = k.simplified().remove(' '); }
    if (doc.contains("openai_model"))   cfg.openaiModel = QString::fromStdString(doc["openai_model"].get<std::string>());
    if (doc.contains("ip_geo_url"))     cfg.ipGeoUrl = QString::fromStdString(doc["ip_geo_url"].get<std::string>());
    if (doc.contains("min_alt_deg"))    cfg.minAltDeg = doc["min_alt_deg"].get<double>();
    if (doc.contains("min_hours_above"))cfg.minHoursAbove = doc["min_hours_above"].get<double>();
    if (doc.contains("page_size"))      cfg.pageSize = doc["page_size"].get<int>();
    if (doc.contains("focal_length_mm") && doc["focal_length_mm"].is_number()) cfg.focalLengthMm = doc["focal_length_mm"].get<double>();
    if (doc.contains("pixel_size_um")  && doc["pixel_size_um"].is_number())    cfg.pixelSizeUm   = doc["pixel_size_um"].get<double>();
    if (doc.contains("sensor_width_px") && doc["sensor_width_px"].is_number_integer())  cfg.sensorWidthPx  = doc["sensor_width_px"].get<int>();
    if (doc.contains("sensor_height_px")&& doc["sensor_height_px"].is_number_integer()) cfg.sensorHeightPx = doc["sensor_height_px"].get<int>();
    if(doc.contains("active_profile")&&doc["active_profile"].is_number_integer())
      cfg.activeProfile=doc["active_profile"].get<int>();
    if(doc.contains("gear_profiles")&&doc["gear_profiles"].is_array())
      for(const auto& pj:doc["gear_profiles"]) cfg.gearProfiles.push_back(profileFromJson(pj));
    if(cfg.gearProfiles.isEmpty()){
      GearProfile p; p.name="Default Setup";
      if(cfg.focalLengthMm)p.focalLenMm=*cfg.focalLengthMm;
      if(cfg.pixelSizeUm)p.pixelUm=*cfg.pixelSizeUm;
      p.widthPx=cfg.sensorWidthPx; p.heightPx=cfg.sensorHeightPx;
      cfg.gearProfiles.push_back(p);
    }
    cfg.activeProfile=std::clamp(cfg.activeProfile,0,(int)cfg.gearProfiles.size()-1);
    cfg.syncFromActiveProfile();
  } catch (...) {}
  return cfg;
}

static void saveConfig(const AppConfig& cfg){
  QDir().mkpath(appDirPath());
  QFile f(configPath());
  if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate))return;
  json doc;
  doc["sqlite_db_path"]=cfg.sqlitePath.toStdString();
  doc["openai_api_key"]=cfg.openaiKey.toStdString();
  doc["openai_model"]=cfg.openaiModel.toStdString();
  doc["ip_geo_url"]=cfg.ipGeoUrl.toStdString();
  doc["min_alt_deg"]=cfg.minAltDeg;
  doc["min_hours_above"]=cfg.minHoursAbove;
  doc["page_size"]=cfg.pageSize;
  doc["active_profile"]=cfg.activeProfile;
  json profiles=json::array();
  for(const auto& p:cfg.gearProfiles)profiles.push_back(profileToJson(p));
  doc["gear_profiles"]=profiles;
  std::string s=doc.dump(2);
  f.write(s.c_str(),s.size());
}

// ============================================================
// CURL
// ============================================================
static size_t curlWrite(void* c, size_t s, size_t n, void* u) {
  static_cast<std::string*>(u)->append((char*)c, s*n); return s*n;
}
static std::optional<std::string> httpGet(const std::string& url) {
  CURL* curl = curl_easy_init(); if (!curl) return {};
  std::string resp;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);  // Increased from 15s to 30s for survey servers
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "AstroToolkit/1.0");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  CURLcode res = curl_easy_perform(curl);
  long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  // Accept success codes; 3xx should auto-redirect with FOLLOWLOCATION
  if (res != CURLE_OK || code < 200 || code >= 300) return {};
  return resp;
}
static std::optional<std::string> httpPostJson(const std::string& url, const std::string& body, const std::vector<std::string>& hdrs) {
  CURL* curl = curl_easy_init(); if (!curl) return {};
  std::string resp;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "AstroToolkit/1.0");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  struct curl_slist* hdr = nullptr;
  for (const auto& h : hdrs) hdr = curl_slist_append(hdr, h.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
  CURLcode res = curl_easy_perform(curl);
  long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (hdr) curl_slist_free_all(hdr);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK || code < 200 || code >= 300) return {};
  return resp;
}

// ============================================================
// Location
// ============================================================
struct LocationFix { double lat=0, lon=0; bool ok=false; QString source; };

static LocationFix getLocationViaGPSD(int timeoutMs=2500) {
  LocationFix out;
  (void)timeoutMs;
#ifdef HAVE_GPSD
  gps_data_t gd;
  if (gps_open("localhost","2947",&gd)!=0) return out;
  gps_stream(&gd,WATCH_ENABLE|WATCH_JSON,nullptr);
  auto start=std::chrono::steady_clock::now();
  while(true){
    if(gps_waiting(&gd,200000)){
      if(gps_read(&gd,nullptr,0)>0){
        const auto&fix=gd.fix;
        if(fix.mode>=MODE_2D&&std::isfinite(fix.latitude)&&std::isfinite(fix.longitude)){
          out.lat=fix.latitude;out.lon=fix.longitude;out.ok=true;out.source="gpsd";break;
        }
      }
    }
    if(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()>timeoutMs)break;
  }
  gps_stream(&gd,WATCH_DISABLE,nullptr);gps_close(&gd);
#endif
  return out;
}
static LocationFix getLocationViaIP(const AppConfig& cfg) {
  LocationFix out;
  auto resp=httpGet(cfg.ipGeoUrl.toStdString()); if(!resp) return out;
  try {
    auto j=json::parse(*resp);
    auto getNum=[&](const std::vector<std::string>&keys)->std::optional<double>{
      for(auto&k:keys) if(j.contains(k)&&j[k].is_number()) return j[k].get<double>();
      return {};
    };
    auto lat=getNum({"latitude","lat"}), lon=getNum({"longitude","lon","lng"});
    if(!lat||!lon) return out;
    out.lat=*lat;out.lon=*lon;out.ok=true;out.source="ip_geo";
  } catch(...){}
  return out;
}
static LocationFix getBestLocation(const AppConfig& cfg) {
  auto g=getLocationViaGPSD(); if(g.ok) return g;
  return getLocationViaIP(cfg);
}

// ============================================================
// Astronomy
// ============================================================
static QDateTime nowUtc() { return QDateTime::currentDateTimeUtc(); }
static QString isoLocal(const QDateTime& dt) { return dt.toLocalTime().toString("yyyy-MM-dd HH:mm"); }
static double julianDateUTC(const QDateTime& utc) {
  return 2440587.5+(double)utc.toSecsSinceEpoch()/86400.0;
}
static double gmstRad(double jd) {
  double T=(jd-2451545.0)/36525.0;
  double g=280.46061837+360.98564736629*(jd-2451545.0)+0.000387933*T*T-(T*T*T)/38710000.0;
  g=std::fmod(g,360.0); if(g<0)g+=360.0; return deg2rad(g);
}
static double lstRad(double jd,double lon){return wrap2pi(gmstRad(jd)+lon);}
static void sunRaDec(double jd,double&ra,double&dec){
  double n=jd-2451545.0,L=deg2rad(std::fmod(280.460+0.9856474*n,360.0)),g=deg2rad(std::fmod(357.528+0.9856003*n,360.0));
  double lam=L+deg2rad(1.915)*sin(g)+deg2rad(0.020)*sin(2*g),eps=deg2rad(23.439-0.0000004*n);
  ra=wrap2pi(atan2(cos(eps)*sin(lam),cos(lam)));dec=asin(sin(eps)*sin(lam));
}
static void altAz(double jd,double lat,double lon,double ra,double dec,double&alt,double&az){
  double ha=wrap2pi(lstRad(jd,lon)-ra); if(ha>M_PI)ha-=2*M_PI;
  double sa=sin(lat)*sin(dec)+cos(lat)*cos(dec)*cos(ha); alt=asin(sa);
  double ca=cos(alt); if(fabs(ca)<1e-9)ca=1e-9;
  az=wrap2pi(atan2(-cos(dec)*sin(ha)/ca,(sin(dec)-sin(alt)*sin(lat))/(ca*cos(lat)+1e-12)));
}
static double sunAltDeg(double jd,double lat,double lon){
  double ra,dec,alt,az; sunRaDec(jd,ra,dec); altAz(jd,lat,lon,ra,dec,alt,az); return rad2deg(alt);
}

struct NightWindow { QDateTime startUtc,endUtc; QVector<QDateTime> gridUtc; };

static std::optional<NightWindow> computeTonightWindow(double latDeg,double lonDeg){
  if(fabs(latDeg)>80.0) return {};
  const double lat=deg2rad(latDeg),lon=deg2rad(lonDeg);
  QDateTime n=nowUtc(), s=n.addSecs(-(n.time().minute()%10)*60), e=s.addSecs(24*3600);
  const int step=600;
  auto build=[&](double thr)->std::optional<NightWindow>{
    QVector<QDateTime> dark;
    for(QDateTime t=s;t<e;t=t.addSecs(step))
      if(sunAltDeg(julianDateUTC(t),lat,lon)<thr) dark.push_back(t);
    if(dark.size()<12) return {};
    QVector<QVector<QDateTime>> blocks; QVector<QDateTime> cur;
    for(int i=0;i<dark.size();i++){
      if(cur.isEmpty())cur.push_back(dark[i]);
      else if(cur.last().secsTo(dark[i])<=step+1)cur.push_back(dark[i]);
      else{blocks.push_back(cur);cur.clear();cur.push_back(dark[i]);}
    }
    if(!cur.isEmpty())blocks.push_back(cur);
    QVector<QDateTime> chosen;
    for(auto&b:blocks)if(b.last()>n){chosen=b;break;}
    if(chosen.isEmpty()){int best=-1,bN=0;for(int i=0;i<blocks.size();i++)if(blocks[i].size()>bN){bN=blocks[i].size();best=i;}if(best>=0)chosen=blocks[best];}
    if(chosen.isEmpty())return {};
    NightWindow w; w.startUtc=chosen.first();w.endUtc=chosen.last();w.gridUtc=chosen; return w;
  };
  auto a=build(-18.0); if(a)return a; return build(-12.0);
}
static double maxAltAtTransitDeg(double lat,double dec){return 90.0-fabs(lat-dec);}
static double maxHoursAboveAlt(double latD,double decD,double altD){
  double lat=deg2rad(latD),dec=deg2rad(decD),alt=deg2rad(altD);
  double num=sin(alt)-sin(lat)*sin(dec), den=cos(lat)*cos(dec);
  if(fabs(den)<1e-12){return fabs(lat-dec)<1e-6?24.0:0.0;}
  double c=num/den;
  if(c<=-1.0) return 24.0;
  if(c>=1.0)  return 0.0;
  return 24.0*acos(c)/M_PI;
}

// ============================================================
// DSO model
// ============================================================
struct DSO {
  QString key,messier,type,constellation;
  double raDeg=0,decDeg=0;
  std::optional<double> vmag,majArcmin,minArcmin;
  double maxAltDeg=-90; QDateTime tMaxUtc;
  double bestRunHoursAbove=0;
  QVector<double> altDegPath,azDegPath;
  QString display()const{return messier.isEmpty()?key:messier+" / "+key;}
};

enum class Category{Galaxies,Nebulae,Clusters,Messier};
static QStringList typesFor(Category c){
  switch(c){
    case Category::Galaxies:return{"G","GPair","GTrpl","GGroup"};
    case Category::Nebulae: return{"HII","EmN","Neb","SNR","PN","Cl+N","RfN","DrkN"};
    case Category::Clusters:return{"OCl","GCl","*Ass"};
    default:return{};
  }
}
static std::optional<std::pair<double,double>> computeFov(const AppConfig&cfg){
  if(!cfg.focalLengthMm||!cfg.pixelSizeUm) return {};
  if(*cfg.focalLengthMm<1||*cfg.pixelSizeUm<0.1) return {};
  double sc=206265.0*(*cfg.pixelSizeUm/1000.0)/ *cfg.focalLengthMm/60.0;
  return std::make_pair(sc*cfg.sensorWidthPx,sc*cfg.sensorHeightPx);
}
static std::optional<double> fovLongAxisArcmin(const AppConfig&cfg){
  auto f=computeFov(cfg); if(!f)return {}; return std::max(f->first,f->second);
}
static double scoreObject(const DSO&o,Category cat){
  // Heavily weight altitude and availability
  double s = o.maxAltDeg * 1.5 + o.bestRunHoursAbove * 15.0;
  
  // Magnitude weighting: brighter is better
  if(o.vmag) s += (12.0 - std::clamp(*o.vmag, -2.0, 12.0)) * 2.0;
  
  // Size weighting: favor larger objects for "showpiece" feel
  if(o.majArcmin) s += std::min(60.0, *o.majArcmin) * 0.5;
  
  // Popularity and catalog bonuses
  if(!o.messier.isEmpty()) s += 25.0; // Significant bonus for Messier objects
  if(cat==Category::Nebulae){
    if(o.type=="EmN"||o.type=="HII") s += 10.0;
    else if(o.type=="PN"||o.type=="SNR") s += 6.0;
  }
  static const QSet<QString> kPop={"M1","M8","M13","M16","M17","M20","M27","M31","M33","M42","M43","M45","M51","M57","M63","M64","M74","M78","M81","M82","M92","M97","M101","M104","M106","M110","NGC7000","NGC1499","NGC2244","NGC2237","NGC6992","NGC6960","NGC6888","NGC0281","IC1805","IC1848","IC5070","IC0434","NGC0891","NGC4565","NGC7331","NGC7293","NGC2238","NGC2239","NGC2246","NGC2174","IC2177","NGC1977","M20","M82","M81"};
  
  // Massive popularity bonus (100 pts) to ensure famous objects always top the list
  bool isFamous = kPop.contains(o.messier) || kPop.contains(o.key);
  if(isFamous) s += 100.0;

  // "Small subnebulosity" penalty: if not famous and size < 10 arcmin
  if(!isFamous && o.messier.isEmpty() && o.majArcmin && *o.majArcmin < 10.0) {
      s -= 50.0;
  }

  return s;
}
static void computeTonightForObject(DSO&o,const NightWindow&win,double latD,double lonD,double minAlt){
  const double lat=deg2rad(latD),lon=deg2rad(lonD),ra=deg2rad(o.raDeg),dec=deg2rad(o.decDeg);
  o.altDegPath.clear();o.azDegPath.clear();o.maxAltDeg=-90;
  if(win.gridUtc.isEmpty()){o.bestRunHoursAbove=0;return;}
  int best=0,cur=0; const int step=600;
  for(const auto&t:win.gridUtc){
    double alt,az; altAz(julianDateUTC(t),lat,lon,ra,dec,alt,az);
    double ad=rad2deg(alt),azd=rad2deg(az);
    o.altDegPath.push_back(ad);o.azDegPath.push_back(azd);
    if(ad>o.maxAltDeg){o.maxAltDeg=ad;o.tMaxUtc=t;}
    if(ad>=minAlt){cur++;best=std::max(best,cur);}else cur=0;
  }
  o.bestRunHoursAbove=(best*step)/3600.0;
}

// ============================================================
// SQLite helpers
// ============================================================
static QString makeConnName(){return "sqlite_"+QUuid::createUuid().toString(QUuid::WithoutBraces);}
static std::optional<QString> openSqliteThreadConn(const QString&db,const QString&conn){
  if(!QSqlDatabase::drivers().contains("QSQLITE"))return QString("Qt SQLite driver missing.");
  if(!QFile::exists(db))return QString("DB not found:\n%1").arg(db);
  QSqlDatabase d=QSqlDatabase::addDatabase("QSQLITE",conn);
  d.setDatabaseName(db);
  if(!d.open())return QString("Failed to open DB:\n%1").arg(d.lastError().text());
  return {};
}
static void closeSqliteThreadConn(const QString&conn){
  {auto d=QSqlDatabase::database(conn,false);if(d.isValid()){d.commit();d.close();}}
  QSqlDatabase::removeDatabase(conn);
}
static std::vector<DSO> fetchCandidatesFromDb(const AppConfig&cfg,Category cat){
  std::vector<DSO> out;
  const QString conn=makeConnName();
  if(openSqliteThreadConn(cfg.sqlitePath,conn)) return out;
  QSqlDatabase db=QSqlDatabase::database(conn); QSqlQuery q(db);
  QString sql;
  if(cat==Category::Messier){
    sql="SELECT key_name,messier,type_code,ra_deg,dec_deg,constellation,vmag,maj_ax_arcmin,min_ax_arcmin FROM dso WHERE messier_num IS NOT NULL AND ra_deg IS NOT NULL AND dec_deg IS NOT NULL ORDER BY messier_num ASC LIMIT 200;";
    q.prepare(sql);
  } else {
    auto types=typesFor(cat); QStringList ph; for(int i=0;i<types.size();i++)ph<<"?";
    sql="SELECT key_name,messier,type_code,ra_deg,dec_deg,constellation,vmag,maj_ax_arcmin,min_ax_arcmin FROM dso WHERE ra_deg IS NOT NULL AND dec_deg IS NOT NULL AND type_code IN ("+ph.join(",")+" ) ORDER BY vmag ASC LIMIT 5000;";
    q.prepare(sql); for(const auto&t:types)q.addBindValue(t);
  }
  if(!q.exec()){q.finish();closeSqliteThreadConn(conn);return out;}
  out.reserve(500);
  while(q.next()){
    DSO o;o.key=q.value(0).toString();o.messier=q.value(1).toString();o.type=q.value(2).toString();
    o.raDeg=q.value(3).toDouble();o.decDeg=q.value(4).toDouble();o.constellation=q.value(5).toString();
    if(!q.value(6).isNull())o.vmag=q.value(6).toDouble();
    if(!q.value(7).isNull())o.majArcmin=q.value(7).toDouble();
    if(!q.value(8).isNull())o.minArcmin=q.value(8).toDouble();
    out.push_back(std::move(o));
  }
  q.finish(); closeSqliteThreadConn(conn); return out;
}
static QString toCatalogKey(const QString&raw){
  static QRegularExpression re(R"(^(NGC|IC)\s*(\d+)$)",QRegularExpression::CaseInsensitiveOption);
  auto m=re.match(raw.trimmed()); if(!m.hasMatch())return{};
  int num=m.captured(2).toInt(); if(num<1||num>9999)return{};
  return m.captured(1).toUpper()+QString("%1").arg(num,4,10,QChar('0'));
}
static std::vector<DSO> fetchSearchFromDb(const AppConfig&cfg,const QString&rawTerm){
  std::vector<DSO> out;
  const QString conn=makeConnName();
  if(openSqliteThreadConn(cfg.sqlitePath,conn)) return out;
  QSqlDatabase db=QSqlDatabase::database(conn); QSqlQuery q(db);
  const QString term=rawTerm.trimmed(), key=toCatalogKey(term);
  QString sql;
  if(!key.isEmpty()){
    sql="SELECT key_name,messier,type_code,ra_deg,dec_deg,constellation,vmag,maj_ax_arcmin,min_ax_arcmin FROM dso WHERE ra_deg IS NOT NULL AND dec_deg IS NOT NULL AND (key_name=? OR messier LIKE ?) ORDER BY vmag ASC LIMIT 200;";
    q.prepare(sql);q.addBindValue(key);q.addBindValue("%"+term+"%");
  } else {
    sql="SELECT key_name,messier,type_code,ra_deg,dec_deg,constellation,vmag,maj_ax_arcmin,min_ax_arcmin FROM dso WHERE ra_deg IS NOT NULL AND dec_deg IS NOT NULL AND (key_name LIKE ? OR messier LIKE ?) ORDER BY vmag ASC LIMIT 200;";
    q.prepare(sql);const QString p="%"+term+"%";q.addBindValue(p);q.addBindValue(p);
  }
  if(!q.exec()){q.finish();closeSqliteThreadConn(conn);return out;}
  while(q.next()){
    DSO o;o.key=q.value(0).toString();o.messier=q.value(1).toString();o.type=q.value(2).toString();
    o.raDeg=q.value(3).toDouble();o.decDeg=q.value(4).toDouble();o.constellation=q.value(5).toString();
    if(!q.value(6).isNull())o.vmag=q.value(6).toDouble();
    if(!q.value(7).isNull())o.majArcmin=q.value(7).toDouble();
    if(!q.value(8).isNull())o.minArcmin=q.value(8).toDouble();
    out.push_back(std::move(o));
  }
  q.finish(); closeSqliteThreadConn(conn); return out;
}

// ============================================================
// OpenAI
// ============================================================
static QString extractOutputText(const json&resp){
  if(resp.contains("choices")&&resp["choices"].is_array()&&!resp["choices"].empty()){
    auto&c=resp["choices"][0];
    if(c.contains("message")&&c["message"].contains("content")&&c["message"]["content"].is_string())
      return QString::fromStdString(c["message"]["content"].get<std::string>());
  }
  if(resp.contains("error")&&resp["error"].contains("message"))
    return "API Error: "+QString::fromStdString(resp["error"]["message"].get<std::string>());
  return "Unexpected response: "+QString::fromStdString(resp.dump(2));
}
static QString buildPlannerPrompt(const DSO&o,double lat,double lon,double minAlt,double minH,const GearProfile*gear=nullptr){
  QString s1=o.majArcmin?QString::number(*o.majArcmin,'f',1):"?";
  QString s2=o.minArcmin?QString::number(*o.minArcmin,'f',1):"?";
  QString equipStr;
  if(gear){
    double fRatio=gear->apertureMm>0?gear->focalLenMm/gear->apertureMm:5.0;
    QString filters; if(gear->hasLRGB)filters+="LRGB"; if(gear->hasHa)filters+="/Hα"; if(gear->hasOIII)filters+="/OIII"; if(gear->hasSII)filters+="/SII";
    equipStr=QString("%1 %2/%3mm (f/%4) + %5, %6")
      .arg(gear->scopeType).arg((int)gear->apertureMm).arg((int)gear->focalLenMm)
      .arg(fRatio,0,'f',1).arg(gear->cameraName.isEmpty()?"mono camera":gear->cameraName).arg(filters.isEmpty()?"LRGB":filters);
  }else{
    equipStr="200/1000mm Newtonian (f/5) + 2\" coma corrector, ToupTek ATR533MM mono, LRGB + SII/Ha/OIII";
  }
  return QString(R"(You are an expert astrophotography planner.
Equipment: %12
Goal: very high quality
Constraints: Target above %1° for ≥ %2h tonight
Location: lat %3, lon %4
Target: %5 (type %6), max alt ~%7° at %8, best window %9h, size %10'×%11'
Return concise:
1) Best strategy (SHO/HOO/LRGB+Ha) and why
2) Subexposure seconds per filter
3) Total integration + % split
4) Gain/offset, dithering, autofocus cadence, key processing tips)").arg(minAlt,0,'f',0).arg(minH,0,'f',1).arg(lat,0,'f',4).arg(lon,0,'f',4).arg(o.display()).arg(o.type).arg(o.maxAltDeg,0,'f',1).arg(isoLocal(o.tMaxUtc)).arg(o.bestRunHoursAbove,0,'f',1).arg(s1).arg(s2).arg(equipStr);
}
static QString callOpenAI(const AppConfig&cfg,const QString&prompt){
  QString key=cfg.openaiKey.trimmed();
  if(key.isEmpty())return "OpenAI key missing. Check config.json";
  if(!key.startsWith("sk-"))return "Invalid key format (must start with sk-)";
  json body; body["model"]=cfg.openaiModel.toStdString();
  body["messages"]=json::array({{{"role","user"},{"content",prompt.toStdString()}}});
  body["max_tokens"]=650; body["temperature"]=0.7;
  std::vector<std::string> hdrs={"Content-Type: application/json","Authorization: Bearer "+key.toStdString()};
  auto resp=httpPostJson("https://api.openai.com/v1/chat/completions",body.dump(),hdrs);
  if(!resp)return "Network error connecting to OpenAI";
  try{return extractOutputText(json::parse(*resp));}
  catch(const std::exception&e){return QString("Parse error: %1").arg(e.what());}
}

// ============================================================
// SkyPlotWidget
// ============================================================
class SkyPlotWidget : public QWidget {
  Q_OBJECT
public:
  explicit SkyPlotWidget(QWidget*p=nullptr):QWidget(p){setMinimumSize(280,280);}
  void setPath(const QVector<double>&a,const QVector<double>&az,const QString&t){alt_=a;az_=az;title_=t;update();}
  void setFovLabel(const QString&l){fovLabel_=l;update();}
protected:
  void paintEvent(QPaintEvent*)override{
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(),QColor(4,8,14));
    int w=width(),h=height(),s=std::min(w,h)-28; if(s<=0)return;
    QPointF c(w/2.0,h/2.0); double R=s/2.0;
    // Atmospheric gradient
    QRadialGradient g(c,R*1.1); g.setColorAt(0,QColor(15,35,65,40)); g.setColorAt(1,QColor(0,0,0,0));
    p.fillRect(rect(),g);
    
    // Altitude Rings
    for(int a:{30,60}){
      double rr=(90.0-a)/90.0*R;
      QPen rp(QColor(40,85,130,120),1,Qt::DashLine); rp.setDashPattern({5,8}); p.setPen(rp); p.setBrush(Qt::NoBrush);
      p.drawEllipse(c,rr,rr);
      double la=deg2rad(42.0);QPointF lp(c.x()+rr*sin(la),c.y()-rr*cos(la));
      p.setFont(QFont("Inter",7));p.setPen(QColor(60,140,200,180));
      p.drawText(QRectF(lp.x()-14,lp.y()-8,28,16),Qt::AlignCenter,QString::number(a)+"°");
    }
    // Horizon
    p.setPen(QPen(QColor(50,120,190),2)); p.setBrush(Qt::NoBrush); p.drawEllipse(c,R,R);
    // Grid
    p.setPen(QPen(QColor(30,70,110,80),1,Qt::DotLine));
    p.drawLine(QPointF(c.x(),c.y()-R),QPointF(c.x(),c.y()+R));
    p.drawLine(QPointF(c.x()-R,c.y()),QPointF(c.x()+R,c.y()));
    // Zenith Marker
    p.setBrush(QColor(0,180,255,100)); p.setPen(QPen(QColor(0,180,255,180),1)); 
    p.drawEllipse(c,3,3);
    
    p.setFont(QFont("Inter",10,QFont::Bold));
    auto dc=[&](const QString&t,double az){
      double a=deg2rad(az),rr=R*1.10;QPointF pt(c.x()+rr*sin(a),c.y()-rr*cos(a));
      QRectF b(pt.x()-14,pt.y()-10,28,20);p.fillRect(b.adjusted(-2,-2,2,2),QColor(6,10,18,200));
      p.setPen(QColor(130,195,240));p.drawText(b,Qt::AlignCenter,t);
    };
    dc("N",0);dc("E",90);dc("S",180);dc("W",270);
    if(alt_.size()>=2&&az_.size()==alt_.size()){
      auto toP=[&](double ad,double azd)->QPointF{
        double rr=(90-std::clamp(ad,-90.0,90.0))/90.0*R,a=deg2rad(azd);
        return QPointF(c.x()+rr*sin(a),c.y()-rr*cos(a));
      };
      QPainterPath pp; pp.moveTo(toP(alt_[0],az_[0]));
      for(int i=1;i<alt_.size();i++)pp.lineTo(toP(alt_[i],az_[i]));
      p.setPen(QPen(QColor(0,150,255,50),12,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(pp);
      p.setPen(QPen(QColor(0,190,255,120),6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(pp);
      p.setPen(QPen(QColor(160,235,255),2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(pp);
      QPointF st=toP(alt_[0],az_[0]);p.setBrush(QColor(0,255,127));p.setPen(QPen(QColor(255,255,255,200),1.5));p.drawEllipse(st,5,5);
      QPointF en=toP(alt_[alt_.size()-1],az_[alt_.size()-1]);p.setBrush(QColor(255,127,0));p.setPen(QPen(QColor(255,255,255,200),1.5));p.drawEllipse(en,5,5);
    }
    p.setFont(QFont("Inter",11,QFont::Bold));
    QRectF tr(0,4,w,26);p.fillRect(tr,QColor(6,10,18,220));p.setPen(QColor(180,225,255));p.drawText(tr,Qt::AlignCenter,title_);
    if(!fovLabel_.isEmpty()){
      p.setFont(QFont("Arial",9,QFont::Bold));QFontMetrics fm(p.font());
      int tw=fm.horizontalAdvance(fovLabel_)+18,th=22;
      QRectF b(w-tw-8,h-th-8,tw,th);
      p.fillRect(b,QColor(0,70,52,200));p.setPen(QPen(QColor(13,112,80),1));p.drawRoundedRect(b,4,4);
      p.setPen(QColor(32,232,192));p.drawText(b,Qt::AlignCenter,fovLabel_);
    }
  }
private:
  QVector<double> alt_,az_; QString title_,fovLabel_;
};

// ============================================================
// FramingWidget — DSS sky survey image + FOV overlay
// ============================================================
class FramingWidget : public QWidget {
  Q_OBJECT
public:
  explicit FramingWidget(QWidget* parent=nullptr):QWidget(parent){
    setMinimumSize(280,280);
  }
  void setTarget(const QString& name,double raDeg,double decDeg,double fovWArcmin,double fovHArcmin){
    targetName_=name; ra_=raDeg; dec_=decDeg; fovW_=fovWArcmin; fovH_=fovHArcmin;
    if(fovWArcmin<1||fovHArcmin<1){
      status_="No FOV — configure equipment profile"; dssImg_={}; update(); return;
    }
    ++reqId_; int myId=reqId_;
    status_="Loading DSS image…"; loading_=true; dssImg_={}; update();
    double reqSize=std::min(std::max(fovWArcmin,fovHArcmin)*2.2,180.0);
    reqSizeArcmin_=reqSize;
    auto fut=QtConcurrent::run([raDeg,decDeg,reqSize]()->QByteArray{
      // Try multiple survey sources with fallback chains
      double sizeDeg=reqSize/60.0;
      
      // Source 1: NASA SkyView — DSS2 Red (most reliable)
      {
        QString url=QString("https://skyview.gsfc.nasa.gov/current/cgi/runquery.pl"
          "?Survey=DSS2%%20Red&Position=%1,%2&Size=%3&Pixels=600&Return=GIF&Coordinates=J2000&cached=yes")
          .arg(raDeg,0,'f',6).arg(decDeg,0,'f',6).arg(sizeDeg,0,'f',4);
        auto res=httpGet(url.toStdString());
        if(res && res->size() > 100){
          const auto& d = *res;
          if(d.compare(0, 3, "GIF") == 0 || 
             (d.size()>1 && d[0]=='\xFF' && d[1]=='\xD8') ||
             (d.size()>3 && d[0]=='\x89' && d[1]=='P' && d[2]=='N' && d[3]=='G'))
            return QByteArray(d.data(), d.size());
        }
      }
      
      // Source 2: STScI DSS2 Red
      {
        QString url=QString("https://archive.stsci.edu/cgi-bin/dss_search"
          "?v=poss2ukstu_red&r=%1&d=%2&e=J2000&h=%3&w=%3&f=gif&c=none")
          .arg(raDeg,0,'f',6).arg(decDeg,0,'f',6).arg(reqSize,0,'f',1);
        auto res=httpGet(url.toStdString());
        if(res && res->size() > 100){
          const auto& d = *res;
          if(d.compare(0, 3, "GIF") == 0 || 
             (d.size()>1 && d[0]=='\xFF' && d[1]=='\xD8') ||
             (d.size()>3 && d[0]=='\x89' && d[1]=='P' && d[2]=='N' && d[3]=='G'))
            return QByteArray(d.data(), d.size());
        }
      }
      
      // Source 3: NASA SkyView — 2MASS K-band (fallback to different survey)
      {
        QString url=QString("https://skyview.gsfc.nasa.gov/current/cgi/runquery.pl"
          "?Survey=2MASS%%20K&Position=%1,%2&Size=%3&Pixels=600&Return=GIF&Coordinates=J2000&cached=yes")
          .arg(raDeg,0,'f',6).arg(decDeg,0,'f',6).arg(sizeDeg,0,'f',4);
        auto res=httpGet(url.toStdString());
        if(res && res->size() > 100){
          const auto& d = *res;
          if(d.compare(0, 3, "GIF") == 0 || 
             (d.size()>1 && d[0]=='\xFF' && d[1]=='\xD8') ||
             (d.size()>3 && d[0]=='\x89' && d[1]=='P' && d[2]=='N' && d[3]=='G'))
            return QByteArray(d.data(), d.size());
        }
      }
      
      // Source 4: NASA SkyView — SDSS fallback
      {
        QString url=QString("https://skyview.gsfc.nasa.gov/current/cgi/runquery.pl"
          "?Survey=SDSSr&Position=%1,%2&Size=%3&Pixels=600&Return=GIF&Coordinates=J2000&cached=yes")
          .arg(raDeg,0,'f',6).arg(decDeg,0,'f',6).arg(sizeDeg,0,'f',4);
        auto res=httpGet(url.toStdString());
        if(res && res->size() > 100){
          const auto& d = *res;
          if(d.compare(0, 3, "GIF") == 0 || 
             (d.size()>1 && d[0]=='\xFF' && d[1]=='\xD8') ||
             (d.size()>3 && d[0]=='\x89' && d[1]=='P' && d[2]=='N' && d[3]=='G'))
            return QByteArray(d.data(), d.size());
        }
      }
      
      return {};
    });
    auto* w=new QFutureWatcher<QByteArray>(this);
    connect(w,&QFutureWatcher<QByteArray>::finished,this,[this,w,myId](){
      QByteArray data=w->result(); w->deleteLater();
      if(myId!=reqId_)return;
      loading_=false;
      if(data.isEmpty()){
        status_="DSS fetch failed\n(all survey servers unavailable)\nCheck network or try again later"; 
        update();
        return;
      }
      QPixmap pm;
      if(!pm.loadFromData(data)){
        status_="Image decode failed\n(unexpected format from server)"; 
        update();
        return;
      }
      dssImg_=pm; status_=""; update();
    });
    w->setFuture(fut);
  }
  void clear(){dssImg_={}; targetName_=""; status_="Select a target to load framing"; loading_=false; update();}
protected:
  void paintEvent(QPaintEvent*)override{
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(),QColor(6,10,18));
    int W=width(),H=height();
    // Title bar
    p.fillRect(0,0,W,26,QColor(6,10,18,230));
    p.setFont(QFont("Arial",11,QFont::Bold));
    p.setPen(QColor(180,225,255));
    QString title=targetName_.isEmpty()?"Framing Diagram":targetName_+" — Framing (DSS2 Red)";
    p.drawText(QRectF(0,4,W,20),Qt::AlignCenter,title);
    if(!status_.isEmpty()){
      p.setFont(QFont("Arial",10)); p.setPen(QColor(120,160,200));
      p.drawText(QRectF(0,26,W,H-26),Qt::AlignCenter,status_);
      return;
    }
    if(dssImg_.isNull())return;
    // Draw image
    int imgH=H-46;
    QPixmap scaled=dssImg_.scaled(W,imgH,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    int ox=(W-scaled.width())/2, oy=28;
    p.drawPixmap(ox,oy,scaled);
    // FOV rectangle overlay
    if(fovW_>0&&fovH_>0&&reqSizeArcmin_>0&&scaled.width()>0&&scaled.height()>0){
      double scX=scaled.width()/reqSizeArcmin_, scY=scaled.height()/reqSizeArcmin_;
      double cx=ox+scaled.width()/2.0, cy=oy+scaled.height()/2.0;
      double rw=fovW_*scX, rh=fovH_*scY;
      QRectF box(cx-rw/2,cy-rh/2,rw,rh);
      // Glow
      p.setPen(QPen(QColor(0,180,255,50),8)); p.setBrush(Qt::NoBrush); p.drawRect(box);
      p.setPen(QPen(QColor(0,210,255,160),2)); p.drawRect(box);
      // Corner ticks
      double cs=std::min(rw,rh)*0.10;
      p.setPen(QPen(QColor(255,255,255,210),2));
      auto tick=[&](double x,double y,double dx,double dy){
        p.drawLine(QPointF(x,y),QPointF(x+dx*cs,y));
        p.drawLine(QPointF(x,y),QPointF(x,y+dy*cs));
      };
      tick(box.left(),box.top(),1,1); tick(box.right(),box.top(),-1,1);
      tick(box.left(),box.bottom(),1,-1); tick(box.right(),box.bottom(),-1,-1);
      // FOV label
      p.setFont(QFont("Arial",8,QFont::Bold)); p.setPen(QColor(0,210,255));
      p.drawText(QRectF(box.left(),box.bottom()+3,rw,14),Qt::AlignHCenter,
        QString("%1' × %2'").arg(fovW_,0,'f',1).arg(fovH_,0,'f',1));
    }
    // Scale bar
    if(reqSizeArcmin_>0&&scaled.width()>0){
      double scX=scaled.width()/reqSizeArcmin_;
      double barAm=std::pow(10.0,std::floor(std::log10(reqSizeArcmin_/4.0)));
      if(barAm<1)barAm=1;
      double barPx=barAm*scX;
      double bx=ox+scaled.width()-barPx-10, by=oy+scaled.height()-12;
      p.setPen(QPen(QColor(220,220,220,200),2));
      p.drawLine(QPointF(bx,by),QPointF(bx+barPx,by));
      p.drawLine(QPointF(bx,by-4),QPointF(bx,by+4));
      p.drawLine(QPointF(bx+barPx,by-4),QPointF(bx+barPx,by+4));
      QString lbl=barAm>=1?QString("%1'").arg((int)barAm):QString("%1\"").arg((int)(barAm*60));
      p.setFont(QFont("Arial",8)); p.setPen(QColor(200,200,200,200));
      p.drawText(QRectF(bx,by-16,barPx,13),Qt::AlignHCenter,lbl);
    }
    // Bottom info bar
    p.fillRect(0,H-18,W,18,QColor(6,10,18,220));
    p.setFont(QFont("Arial",8)); p.setPen(QColor(90,140,190));
    p.drawText(QRectF(6,H-15,W-12,13),Qt::AlignLeft,
      QString("Context: %1'×%1'  |  Survey: DSS2 Red (multi-source)").arg(reqSizeArcmin_,0,'f',0));
  }
private:
  QPixmap dssImg_;
  QString targetName_, status_="Select a target to load framing";
  double ra_=0,dec_=0,fovW_=0,fovH_=0,reqSizeArcmin_=0;
  bool loading_=false;
  int reqId_=0;
};

// ============================================================
// GearProfileDialog
// ============================================================
class GearProfileDialog : public QDialog {
  Q_OBJECT
public:
  explicit GearProfileDialog(const AppConfig& cfg,QWidget* parent=nullptr)
    :QDialog(parent),editProfiles_(cfg.gearProfiles),activeIdx_(cfg.activeProfile){
    setWindowTitle("Equipment Profiles");
    resize(820,560);
    const QString es="QWidget{background:#07101c;color:#b8d0e8;}"
      "QLineEdit,QDoubleSpinBox,QSpinBox,QComboBox{background:#0b1828;color:#c8e0f8;border:1px solid #1a4060;border-radius:4px;padding:3px 6px;}"
      "QCheckBox{color:#90b8d4;spacing:6px;}"
      "QCheckBox::indicator{width:14px;height:14px;border:1px solid #1a4a6a;border-radius:3px;background:#0b1828;}"
      "QCheckBox::indicator:checked{background:#0a7acc;border-color:#00aeff;}"
      "QPushButton{background:#0e2035;color:#60c8f0;border:1px solid #1a4060;border-radius:4px;padding:4px 10px;}"
      "QPushButton:hover{background:#1a3050;border-color:#2a6090;}"
      "QListWidget{background:#080e18;color:#b8d0e8;border:1px solid #1a3650;border-radius:4px;}"
      "QListWidget::item:selected{background:#1a3a5a;color:#e0f0ff;}"
      "QLabel{color:#90b0cc;}"
      "QScrollArea{border:none;}";
    setStyleSheet(es);

    // Left: profile list
    profileList_=new QListWidget; profileList_->setFixedWidth(210);
    auto* addBtn=new QPushButton("+ New");
    auto* dupBtn=new QPushButton("Duplicate");
    auto* delBtn=new QPushButton("Delete");
    auto* btnRow=new QHBoxLayout; btnRow->setSpacing(4);
    btnRow->addWidget(addBtn);btnRow->addWidget(dupBtn);btnRow->addWidget(delBtn);
    auto* leftW=new QWidget; auto* leftV=new QVBoxLayout(leftW);
    leftV->setContentsMargins(0,0,4,0);
    auto* listTitleLbl=new QLabel("Saved Profiles");
    listTitleLbl->setStyleSheet("color:#60c8f0;font-weight:bold;padding:0 0 4px 0;");
    leftV->addWidget(listTitleLbl);
    leftV->addWidget(profileList_,1);
    leftV->addLayout(btnRow);

    // Right: form
    nameEdit_=new QLineEdit;
    scopeTypeCombo_=new QComboBox; for(const auto& t:kScopeTypes)scopeTypeCombo_->addItem(t);
    focalEdit_=new QDoubleSpinBox; focalEdit_->setRange(100,10000); focalEdit_->setSuffix(" mm"); focalEdit_->setDecimals(0);
    apertureEdit_=new QDoubleSpinBox; apertureEdit_->setRange(20,2000); apertureEdit_->setSuffix(" mm"); apertureEdit_->setDecimals(0);
    camNameEdit_=new QLineEdit; camNameEdit_->setPlaceholderText("e.g. ZWO ASI533MM");
    pixelEdit_=new QDoubleSpinBox; pixelEdit_->setRange(0.5,30); pixelEdit_->setSuffix(" µm"); pixelEdit_->setDecimals(2);
    widthEdit_=new QSpinBox; widthEdit_->setRange(100,20000); widthEdit_->setSuffix(" px");
    heightEdit_=new QSpinBox; heightEdit_->setRange(100,20000); heightEdit_->setSuffix(" px");
    fovLbl_=new QLabel("FOV: —");
    fovLbl_->setStyleSheet("color:#20e8c0;font-weight:bold;padding:4px 10px;background:rgba(0,60,40,0.5);border:1px solid #0a5030;border-radius:4px;");
    lrgbCk_=new QCheckBox("LRGB"); haCk_=new QCheckBox("Hα");
    oiiiCk_=new QCheckBox("OIII"); siiCk_=new QCheckBox("SII");

    // Preset menu
    auto* presetBtn=new QPushButton("Load Preset ▾");
    auto* menu=new QMenu(this);
    for(const auto& preset:builtinPresets()){
      menu->addAction(preset.name,this,[this,preset](){
        int r=profileList_->currentRow(); if(r<0)return;
        block_=true; setFormFromProfile(preset); block_=false;
        saveFormToProfile(r); refreshLabels();
      });
    }
    presetBtn->setMenu(menu);

    // Build form grid
    auto mkSep=[](const QString& t)->QLabel*{
      auto* l=new QLabel(t);
      l->setStyleSheet("color:#60c8f0;font-weight:bold;padding:6px 0 2px 0;border-bottom:1px solid #1a3650;");
      return l;
    };
    auto* formW=new QWidget;
    auto* g=new QGridLayout(formW); g->setSpacing(8); g->setContentsMargins(10,8,10,8);
    int r=0;
    g->addWidget(new QLabel("Profile Name:"),r,0,Qt::AlignRight); g->addWidget(nameEdit_,r,1,1,3); r++;
    g->addWidget(mkSep("Telescope"),r,0,1,4); r++;
    g->addWidget(new QLabel("Type:"),r,0,Qt::AlignRight); g->addWidget(scopeTypeCombo_,r,1);
    g->addWidget(new QLabel("Aperture:"),r,2,Qt::AlignRight); g->addWidget(apertureEdit_,r,3); r++;
    g->addWidget(new QLabel("Focal Length:"),r,0,Qt::AlignRight); g->addWidget(focalEdit_,r,1); r++;
    g->addWidget(mkSep("Camera"),r,0,1,4); r++;
    g->addWidget(new QLabel("Camera Name:"),r,0,Qt::AlignRight); g->addWidget(camNameEdit_,r,1,1,3); r++;
    g->addWidget(new QLabel("Pixel Size:"),r,0,Qt::AlignRight); g->addWidget(pixelEdit_,r,1);
    g->addWidget(new QLabel("Sensor:"),r,2,Qt::AlignRight);
    auto* senRow=new QHBoxLayout; senRow->setSpacing(4);
    senRow->addWidget(widthEdit_); senRow->addWidget(new QLabel("×")); senRow->addWidget(heightEdit_);
    auto* senW=new QWidget; senW->setLayout(senRow); g->addWidget(senW,r,3); r++;
    g->addWidget(fovLbl_,r,0,1,4,Qt::AlignLeft); r++;
    g->addWidget(mkSep("Available Filters"),r,0,1,4); r++;
    auto* filtRow=new QHBoxLayout; filtRow->setSpacing(12);
    filtRow->addWidget(lrgbCk_);filtRow->addWidget(haCk_);filtRow->addWidget(oiiiCk_);filtRow->addWidget(siiCk_);filtRow->addStretch();
    auto* filtW=new QWidget; filtW->setLayout(filtRow); g->addWidget(filtW,r,0,1,4); r++;
    g->addWidget(presetBtn,r,0,1,2,Qt::AlignLeft); r++;
    g->setRowStretch(r,1);
    g->setColumnStretch(1,1); g->setColumnStretch(3,1);

    auto* scroll=new QScrollArea; scroll->setWidget(formW); scroll->setWidgetResizable(true);
    auto* rightW=new QWidget; auto* rightV=new QVBoxLayout(rightW);
    rightV->setContentsMargins(4,0,0,0); rightV->addWidget(scroll,1);

    auto* splitter=new QSplitter(Qt::Horizontal);
    splitter->addWidget(leftW); splitter->addWidget(rightW);
    splitter->setSizes({220,580}); splitter->setHandleWidth(4);

    auto* okBtn=new QPushButton("OK"); auto* cancelBtn=new QPushButton("Cancel");
    okBtn->setMinimumHeight(34); cancelBtn->setMinimumHeight(34);
    auto* botRow=new QHBoxLayout;
    botRow->addStretch(1); botRow->addWidget(cancelBtn); botRow->addWidget(okBtn);

    auto* root=new QVBoxLayout(this);
    root->addWidget(splitter,1); root->addLayout(botRow);

    // Populate list
    for(const auto& p:editProfiles_) profileList_->addItem(p.name.isEmpty()?"(unnamed)":p.name);
    if(!editProfiles_.isEmpty()){
      profileList_->setCurrentRow(std::clamp(activeIdx_,0,(int)editProfiles_.size()-1));
    }

    // Live FOV update
    auto updateFov=[this](){
      double fl=focalEdit_->value(),px=pixelEdit_->value();
      int w=widthEdit_->value(),h=heightEdit_->value();
      if(fl>0&&px>0){
        double sc=206265.0*(px/1000.0)/fl/60.0;
        fovLbl_->setText(QString("Computed FOV: %1' × %2'  (%3\" / px)").arg(sc*w,0,'f',1).arg(sc*h,0,'f',1).arg(206265.0*(px/1000.0)/fl,0,'f',2));
      }else fovLbl_->setText("FOV: —");
      int rr=profileList_->currentRow(); if(!block_&&rr>=0&&rr<(int)editProfiles_.size()){saveFormToProfile(rr);refreshLabels();}
    };
    auto onChange=[this](){int rr=profileList_->currentRow();if(!block_&&rr>=0&&rr<(int)editProfiles_.size()){saveFormToProfile(rr);refreshLabels();}};
    connect(focalEdit_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,updateFov);
    connect(pixelEdit_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,updateFov);
    connect(widthEdit_,QOverload<int>::of(&QSpinBox::valueChanged),this,updateFov);
    connect(heightEdit_,QOverload<int>::of(&QSpinBox::valueChanged),this,updateFov);
    connect(nameEdit_,&QLineEdit::textChanged,this,onChange);
    connect(camNameEdit_,&QLineEdit::textChanged,this,onChange);
    connect(scopeTypeCombo_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,onChange);
    connect(lrgbCk_,&QCheckBox::toggled,this,onChange);
    connect(haCk_,&QCheckBox::toggled,this,onChange);
    connect(oiiiCk_,&QCheckBox::toggled,this,onChange);
    connect(siiCk_,&QCheckBox::toggled,this,onChange);
    connect(apertureEdit_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,onChange);

    connect(profileList_,&QListWidget::currentRowChanged,this,[this](int rr){
      if(rr>=0&&rr<(int)editProfiles_.size())loadProfile(rr);
    });
    connect(addBtn,&QPushButton::clicked,this,[this](){
      GearProfile p; p.name="New Profile";
      editProfiles_.push_back(p); profileList_->addItem("New Profile");
      profileList_->setCurrentRow((int)editProfiles_.size()-1);
    });
    connect(dupBtn,&QPushButton::clicked,this,[this](){
      int rr=profileList_->currentRow(); if(rr<0||rr>=(int)editProfiles_.size())return;
      GearProfile p=editProfiles_[rr]; p.name+=" (copy)";
      editProfiles_.push_back(p);
      profileList_->addItem(p.name.isEmpty()?"(unnamed)":p.name);
      profileList_->setCurrentRow((int)editProfiles_.size()-1);
    });
    connect(delBtn,&QPushButton::clicked,this,[this](){
      int rr=profileList_->currentRow(); if(rr<0||rr>=(int)editProfiles_.size())return;
      if(editProfiles_.size()==1){QMessageBox::warning(this,"","Keep at least one profile.");return;}
      editProfiles_.erase(editProfiles_.begin()+rr);
      delete profileList_->takeItem(rr);
      profileList_->setCurrentRow(std::min(rr,(int)editProfiles_.size()-1));
    });
    connect(okBtn,&QPushButton::clicked,this,&QDialog::accept);
    connect(cancelBtn,&QPushButton::clicked,this,&QDialog::reject);
  }
  const QVector<GearProfile>& profiles()const{return editProfiles_;}
  int selectedIndex()const{return profileList_?profileList_->currentRow():0;}

private:
  void loadProfile(int i){
    if(i<0||i>=(int)editProfiles_.size())return;
    const GearProfile& p=editProfiles_[i];
    block_=true;
    nameEdit_->setText(p.name);
    int idx=kScopeTypes.indexOf(p.scopeType); scopeTypeCombo_->setCurrentIndex(idx>=0?idx:0);
    focalEdit_->setValue(p.focalLenMm); apertureEdit_->setValue(p.apertureMm);
    camNameEdit_->setText(p.cameraName);
    pixelEdit_->setValue(p.pixelUm); widthEdit_->setValue(p.widthPx); heightEdit_->setValue(p.heightPx);
    lrgbCk_->setChecked(p.hasLRGB); haCk_->setChecked(p.hasHa); oiiiCk_->setChecked(p.hasOIII); siiCk_->setChecked(p.hasSII);
    block_=false;
    double sc=206265.0*(p.pixelUm/1000.0)/p.focalLenMm/60.0;
    fovLbl_->setText(QString("Computed FOV: %1' × %2'  (%3\" / px)").arg(sc*p.widthPx,0,'f',1).arg(sc*p.heightPx,0,'f',1).arg(206265.0*(p.pixelUm/1000.0)/p.focalLenMm,0,'f',2));
  }
  void saveFormToProfile(int i){
    if(i<0||i>=(int)editProfiles_.size())return;
    GearProfile& p=editProfiles_[i];
    p.name=nameEdit_->text();
    p.scopeType=kScopeTypes.value(scopeTypeCombo_->currentIndex(),"Newtonian");
    p.focalLenMm=focalEdit_->value(); p.apertureMm=apertureEdit_->value();
    p.cameraName=camNameEdit_->text();
    p.pixelUm=pixelEdit_->value(); p.widthPx=widthEdit_->value(); p.heightPx=heightEdit_->value();
    p.hasLRGB=lrgbCk_->isChecked(); p.hasHa=haCk_->isChecked(); p.hasOIII=oiiiCk_->isChecked(); p.hasSII=siiCk_->isChecked();
  }
  void setFormFromProfile(const GearProfile& p){
    nameEdit_->setText(p.name);
    int idx=kScopeTypes.indexOf(p.scopeType); scopeTypeCombo_->setCurrentIndex(idx>=0?idx:0);
    focalEdit_->setValue(p.focalLenMm); apertureEdit_->setValue(p.apertureMm);
    camNameEdit_->setText(p.cameraName);
    pixelEdit_->setValue(p.pixelUm); widthEdit_->setValue(p.widthPx); heightEdit_->setValue(p.heightPx);
    lrgbCk_->setChecked(p.hasLRGB); haCk_->setChecked(p.hasHa); oiiiCk_->setChecked(p.hasOIII); siiCk_->setChecked(p.hasSII);
  }
  void refreshLabels(){
    for(int i=0;i<(int)editProfiles_.size();i++){
      auto* item=profileList_->item(i);
      if(item)item->setText(editProfiles_[i].name.isEmpty()?"(unnamed)":editProfiles_[i].name);
    }
  }
  QVector<GearProfile> editProfiles_;
  int activeIdx_=0;
  QListWidget* profileList_=nullptr;
  QLineEdit* nameEdit_=nullptr, *camNameEdit_=nullptr;
  QComboBox* scopeTypeCombo_=nullptr;
  QDoubleSpinBox* focalEdit_=nullptr, *apertureEdit_=nullptr, *pixelEdit_=nullptr;
  QSpinBox* widthEdit_=nullptr, *heightEdit_=nullptr;
  QLabel* fovLbl_=nullptr;
  QCheckBox* lrgbCk_=nullptr, *haCk_=nullptr, *oiiiCk_=nullptr, *siiCk_=nullptr;
  bool block_=false;
};

// ============================================================
// FITS Viewer — helpers
// ============================================================
static QImage applyStretchQt(const std::vector<float>& buf, int nx, int ny,
                              const QString& mode, float lo, float hi)
{
    if (hi <= lo) hi = lo + 1.0f;
    QImage img(nx, ny, QImage::Format_Grayscale8);
    for (int y = 0; y < ny; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < nx; ++x) {
            float v = (buf[(size_t)y * nx + x] - lo) / (hi - lo);
            v = std::clamp(v, 0.0f, 1.0f);
            if      (mode == "sqrt")  v = std::sqrt(v);
            else if (mode == "log")   v = std::log1p(v * 99.0f) / std::log1p(99.0f);
            else if (mode == "asinh") {
                const float b = 0.05f, d = std::asinh(1.0f / b);
                v = std::clamp(std::asinh(v / b) / d, 0.0f, 1.0f);
            }
            line[x] = (uchar)std::clamp(v * 255.0f, 0.0f, 255.0f);
        }
    }
    return img;
}

// Simple bilinear Bayer demosaicing
static QImage debayerSimple(const QImage& gray, const QString& pat)
{
    int nx = gray.width(), ny = gray.height();
    // ch[row0col0, row0col1, row1col0, row1col1] — 0=R,1=G,2=B
    int ch[4] = {1,1,1,1};
    if      (pat=="RGGB"||pat=="rggb") { ch[0]=0;ch[1]=1;ch[2]=1;ch[3]=2; }
    else if (pat=="BGGR"||pat=="bggr") { ch[0]=2;ch[1]=1;ch[2]=1;ch[3]=0; }
    else if (pat=="GRBG"||pat=="grbg") { ch[0]=1;ch[1]=0;ch[2]=2;ch[3]=1; }
    else if (pat=="GBRG"||pat=="gbrg") { ch[0]=1;ch[1]=2;ch[2]=0;ch[3]=1; }

    QImage rgb(nx, ny, QImage::Format_RGB888);
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            int py = y & ~1, px = x & ~1;
            auto gs = [&](int iy, int ix) -> int {
                if (iy<0||iy>=ny||ix<0||ix>=nx) return 0;
                return gray.constScanLine(iy)[ix];
            };
            int vals[3] = {0,0,0};
            vals[ch[0]] = gs(py,   px);
            vals[ch[1]] = gs(py,   px+1);
            vals[ch[2]] = gs(py+1, px);
            if (ch[1] == ch[2])
                vals[ch[1]] = (gs(py,px+1) + gs(py+1,px)) / 2;
            else
                vals[ch[3]] = gs(py+1, px+1);
            uchar* px3 = rgb.scanLine(y) + x * 3;
            px3[0] = (uchar)vals[0]; px3[1] = (uchar)vals[1]; px3[2] = (uchar)vals[2];
        }
    }
    return rgb;
}

struct FitsLoadResult {
    QImage   image;
    QStringList headerLines;
    QString  statsLine;
    QString  errorMsg;
};

static FitsLoadResult loadFitsImage(const QString& path, const QString& stretchMode)
{
    FitsLoadResult res;
    fitsfile* fptr = nullptr;
    int status = 0;

    fits_open_file(&fptr, path.toLocal8Bit().constData(), READONLY, &status);
    if (status) {
        char msg[FLEN_ERRMSG]; fits_get_errstatus(status, msg);
        res.errorMsg = QString("Cannot open: %1").arg(msg);
        return res;
    }

    int naxis = 0; fits_get_img_dim(fptr, &naxis, &status);
    long naxes[3] = {1,1,1};
    fits_get_img_size(fptr, std::min(naxis,3), naxes, &status);
    long nx = naxes[0], ny = naxes[1], npix = nx * ny;

    if (npix <= 0) { res.errorMsg = "Empty image"; fits_close_file(fptr,&status); return res; }

    std::vector<float> buf((size_t)npix);
    long fpx[3]={1,1,1}; float nv=0; int an=0;
    fits_read_pix(fptr, TFLOAT, fpx, npix, &nv, buf.data(), &an, &status);
    if (status) {
        char msg[FLEN_ERRMSG]; fits_get_errstatus(status, msg);
        res.errorMsg = QString("Read error: %1").arg(msg);
        fits_close_file(fptr,&status); return res;
    }

    // stats
    float mn=buf[0],mx=buf[0]; double sum=0,sum2=0;
    for (float v:buf){ if(v<mn)mn=v;if(v>mx)mx=v;sum+=v;sum2+=(double)v*v; }
    float mean=(float)(sum/npix), std_=(float)std::sqrt(std::max(0.0,sum2/npix-(sum/npix)*(sum/npix)));
    res.statsLine = QString("min:%1  max:%2  mean:%3  std:%4")
        .arg(mn,0,'f',0).arg(mx,0,'f',0).arg(mean,0,'f',0).arg(std_,0,'f',0);

    // percentile bounds — nth_element is O(n) vs O(n log n) full sort
    std::vector<float> sorted = buf;
    long loIdx = std::max(0L, (long)(npix * 0.005));
    long hiIdx = std::min(npix-1L, (long)(npix * 0.995));
    std::nth_element(sorted.begin(), sorted.begin() + loIdx, sorted.end());
    float lo = sorted[loIdx];
    std::nth_element(sorted.begin() + loIdx + 1, sorted.begin() + hiIdx, sorted.end());
    float hi = sorted[hiIdx];

    QImage gray = applyStretchQt(buf, (int)nx, (int)ny, stretchMode, lo, hi);

    // Bayer debayer
    char bayval[FLEN_VALUE]=""; int bs=0;
    fits_read_key(fptr, TSTRING, "BAYERPAT", bayval, nullptr, &bs);
    QString pat = QString(bayval).trimmed().toUpper();
    if (!pat.isEmpty() && pat!="_" )
        res.image = debayerSimple(gray, pat);
    else
        res.image = gray;

    // header
    auto hget = [&](const char* k) -> QString {
        char v[FLEN_VALUE]=""; int s=0;
        fits_read_key(fptr, TSTRING, k, v, nullptr, &s);
        return s==0 ? QString(v).trimmed() : QString();
    };
    res.headerLines = {
        QString("OBJECT:   %1").arg(hget("OBJECT")),
        QString("EXPTIME:  %1 s").arg(hget("EXPTIME").isEmpty() ? hget("EXPOSURE") : hget("EXPTIME")),
        QString("GAIN:     %1").arg(hget("GAIN")),
        QString("FILTER:   %1").arg(hget("FILTER")),
        QString("DATE-OBS: %1").arg(hget("DATE-OBS").isEmpty() ? hget("DATE") : hget("DATE-OBS")),
    };

    fits_close_file(fptr, &status);
    return res;
}

// ============================================================
// Auto-review: star detection for light-frame quality checks
// ============================================================
struct StarDetectionResult {
    int  starCount    = 0;
    int  trailCount   = 0;
    bool hasTrails    = false;
    bool isOutOfFocus = false;
};

static StarDetectionResult detectStarsInBuf(const std::vector<float>& buf, int nx, int ny)
{
    StarDetectionResult res;
    long npix = (long)nx * ny;
    if (npix < 100) return res;

    // Background + sigma via sampled median / MAD
    int step = std::max(1, (int)std::sqrt((double)npix / 50000.0));
    std::vector<float> samp;
    samp.reserve(npix / step + 1);
    for (long i = 0; i < npix; i += step) samp.push_back(buf[i]);
    std::sort(samp.begin(), samp.end());
    float bg = samp[samp.size() / 2];

    std::vector<float> devs;
    devs.reserve(samp.size());
    for (float v : samp) devs.push_back(std::abs(v - bg));
    std::sort(devs.begin(), devs.end());
    float sigma = std::max(1.0f, devs[devs.size() / 2] * 1.4826f);

    float thresh = bg + 5.0f * sigma;

    // Binary mask of bright pixels
    std::vector<bool> mask(npix, false);
    for (long i = 0; i < npix; i++) mask[i] = buf[i] > thresh;

    // 4-connected DFS to label blobs
    std::vector<bool> visited(npix, false);
    std::vector<std::pair<int,int>> stk;
    stk.reserve(4096);

    static const int DX[4] = {1,-1,0,0};
    static const int DY[4] = {0,0,1,-1};

    for (int y = 0; y < ny; y++) {
        for (int x = 0; x < nx; x++) {
            int idx = y * nx + x;
            if (!mask[idx] || visited[idx]) continue;

            stk.clear();
            stk.push_back({x, y});
            visited[idx] = true;
            int area = 0;
            int minX = x, maxX = x, minY = y, maxY = y;

            while (!stk.empty()) {
                auto [cx, cy] = stk.back(); stk.pop_back();
                ++area;
                if (area > 200000) { area = 200000; break; } // cap runaway blobs
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cy < minY) minY = cy;
                if (cy > maxY) maxY = cy;
                for (int d = 0; d < 4; d++) {
                    int nx2 = cx + DX[d], ny2 = cy + DY[d];
                    if (nx2 < 0 || nx2 >= nx || ny2 < 0 || ny2 >= ny) continue;
                    int idx2 = ny2 * nx + nx2;
                    if (!visited[idx2] && mask[idx2]) { visited[idx2] = true; stk.push_back({nx2, ny2}); }
                }
            }

            if (area < 3) continue;

            int bboxW = maxX - minX + 1;
            int bboxH = maxY - minY + 1;
            float longer  = (float)std::max(bboxW, bboxH);
            float shorter = (float)std::max(1, std::min(bboxW, bboxH));
            float aspect  = longer / shorter;

            if (aspect > 4.5f) {
                res.trailCount++;
                res.hasTrails = true;
            } else if (longer > 50.0f) {
                res.isOutOfFocus = true;
            } else if (area <= 2000) {
                res.starCount++;
            }
        }
    }
    return res;
}

struct AutoReviewItem {
    QString path;
    bool    isCalib         = false;
    int     starCount       = 0;
    bool    hasTrails       = false;
    bool    isOutOfFocus    = false;
    bool    deleteSuggested = false;
    QString reason;
};

static AutoReviewItem analyzeFrame(const QString& path)
{
    AutoReviewItem item;
    item.path = path;

    fitsfile* fp = nullptr; int st = 0;
    fits_open_file(&fp, path.toLocal8Bit().constData(), READONLY, &st);
    if (st) { item.reason = "Cannot open file"; return item; }

    // Calibration frame check via IMAGETYP header
    char v[FLEN_VALUE] = ""; int s = 0;
    fits_read_key(fp, TSTRING, "IMAGETYP", v, nullptr, &s);
    if (s == 0) {
        QString t = QString(v).toLower().trimmed();
        if (t.contains("bias") || t.contains("dark") || t.contains("flat")) {
            item.isCalib = true;
            fits_close_file(fp, &st);
            return item;
        }
    }

    // Read pixels
    int naxis = 0; fits_get_img_dim(fp, &naxis, &st);
    long naxes[3] = {1,1,1};
    fits_get_img_size(fp, std::min(naxis, 3), naxes, &st);
    long nx = naxes[0], ny = naxes[1], npix = nx * ny;
    if (npix <= 0) {
        fits_close_file(fp, &st);
        item.reason = "Empty image"; item.deleteSuggested = true; return item;
    }

    std::vector<float> buf((size_t)npix);
    long fpx[3] = {1,1,1}; float nv = 0; int an = 0;
    fits_read_pix(fp, TFLOAT, fpx, npix, &nv, buf.data(), &an, &st);
    fits_close_file(fp, &st);
    if (st) { item.reason = "Read error"; return item; }

    auto det = detectStarsInBuf(buf, (int)nx, (int)ny);
    item.starCount    = det.starCount;
    item.hasTrails    = det.hasTrails;
    item.isOutOfFocus = det.isOutOfFocus;

    QStringList reasons;
    if (det.hasTrails)    reasons << QString("star trails (%1 source%2)").arg(det.trailCount).arg(det.trailCount==1?"":"s");
    if (det.isOutOfFocus) reasons << "out of focus";
    if (det.starCount < 10 && !det.isOutOfFocus)
        reasons << QString("only %1 star%2 detected").arg(det.starCount).arg(det.starCount==1?"":"s");

    if (!reasons.isEmpty()) {
        item.deleteSuggested = true;
        item.reason = reasons.join(", ");
    }
    return item;
}

// ============================================================
// FitsViewerWidget — image canvas with keyboard shortcuts
// ============================================================
class FitsViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit FitsViewerWidget(QWidget* parent=nullptr) : QWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(400,300);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void setContent(const QPixmap& pm, const QStringList& hdr,
                    const QString& stats, const QString& title) {
        pixmap_=pm; hudLines_=hdr; statsLine_=stats; title_=title; update();
    }
    void setError(const QString& msg) {
        pixmap_={}; hudLines_.clear(); statsLine_.clear(); title_=msg; update();
    }
    void clearAll() { pixmap_={}; hudLines_.clear(); statsLine_.clear(); title_.clear(); update(); }

signals:
    void nextRequested();
    void prevRequested();
    void deleteRequested();
    void stretchCycleRequested();

protected:
    void keyPressEvent(QKeyEvent* e) override {
        switch(e->key()){
        case Qt::Key_Right: case Qt::Key_L: case Qt::Key_N: emit nextRequested(); break;
        case Qt::Key_Left:  case Qt::Key_H: case Qt::Key_P: emit prevRequested(); break;
        case Qt::Key_S:     emit stretchCycleRequested(); break;
        case Qt::Key_Delete:case Qt::Key_X: emit deleteRequested(); break;
        default: QWidget::keyPressEvent(e);
        }
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(8,12,20));
        if (!pixmap_.isNull()) {
            QSize sc = pixmap_.size().scaled(size(), Qt::KeepAspectRatio);
            QPoint off((width()-sc.width())/2, (height()-sc.height())/2);
            p.drawPixmap(QRect(off,sc), pixmap_, pixmap_.rect());
        }
        if (!title_.isEmpty()) drawBox(p, {title_}, 10, 10, {255,255,255});
        if (!hudLines_.isEmpty()) drawBox(p, hudLines_, 10, 46, {180,220,255});
        if (!statsLine_.isEmpty()) {
            QStringList bt = { statsLine_,
                "← h p: prev    → l n: next    s: stretch    x Del: delete (permanent)" };
            QFont f("Monospace",8); QFontMetrics fm(f);
            int bh = fm.height()*bt.size() + 20;
            drawBox(p, bt, 10, height()-bh-8, {160,160,160});
        }
    }
private:
    void drawBox(QPainter& p, const QStringList& lines, int x, int y, QColor tc) {
        if(lines.isEmpty()) return;
        QFont f("Monospace",9); p.setFont(f); QFontMetrics fm(f);
        int lh=fm.height()+4, maxW=0;
        for(const auto&l:lines) maxW=std::max(maxW, fm.horizontalAdvance(l));
        int bw=maxW+20, bh=lh*lines.size()+12;
        if(x+bw>width()-4) x=width()-bw-4;
        p.setBrush(QColor(0,0,0,170)); p.setPen(Qt::NoPen);
        p.drawRect(x,y,bw,bh);
        p.setPen(tc); int ty=y+8+fm.ascent();
        for(const auto&l:lines){ p.drawText(x+10,ty,l); ty+=lh; }
    }
    QPixmap pixmap_; QStringList hudLines_; QString statsLine_, title_;
};

// ============================================================
// FitsReviewerPanel
// ============================================================
class FitsReviewerPanel : public QWidget {
    Q_OBJECT
public:
    explicit FitsReviewerPanel(QWidget* parent=nullptr) : QWidget(parent) { setupUI(); }

private:
    void setupUI() {
        auto* vbox = new QVBoxLayout(this);
        vbox->setSpacing(8); vbox->setContentsMargins(8,8,8,8);

        // ---- folder row ----
        auto* folderRow = new QHBoxLayout;
        folderEdit_ = new QLineEdit;
        folderEdit_->setPlaceholderText("Select folder containing FITS files…");
        folderEdit_->setMinimumHeight(32);
        auto* browseBtn = new QPushButton("Browse…");
        browseBtn->setFixedWidth(90); browseBtn->setMinimumHeight(32);
        auto* openBtn = new QPushButton("Open");
        openBtn->setFixedWidth(70); openBtn->setMinimumHeight(32);
        autoReviewBtn_ = new QPushButton("⚡ Auto Review…");
        autoReviewBtn_->setMinimumHeight(32);
        autoReviewBtn_->setToolTip("Scan all loaded frames: delete out-of-focus, star trails and frames with <10 stars");
        autoReviewBtn_->setStyleSheet(
            "QPushButton{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #1a5c30,stop:1 #0e3a1e);"
            "color:#80ffaa;border:1px solid #2a8040;border-radius:5px;font-weight:bold;padding:4px 14px;}"
            "QPushButton:hover{background:#1e7038;color:#ccffdd;border-color:#40c060;}"
            "QPushButton:disabled{background:#0c1a10;color:#2a5030;border-color:#1a2a1a;}");
        folderRow->addWidget(new QLabel("Folder:"));
        folderRow->addWidget(folderEdit_,1);
        folderRow->addWidget(browseBtn);
        folderRow->addWidget(openBtn);
        folderRow->addSpacing(6);
        folderRow->addWidget(autoReviewBtn_);

        // ---- splitter: file list | viewer ----
        auto* splitter = new QSplitter(Qt::Horizontal);
        auto* leftPanel = new QWidget;
        auto* lv = new QVBoxLayout(leftPanel);
        lv->setSpacing(4); lv->setContentsMargins(0,0,0,0);
        statusLbl_ = new QLabel("No folder open");
        statusLbl_->setStyleSheet("QLabel{color:#5a90b0;font-style:italic;padding:2px;}");
        fileList_ = new QListWidget;
        fileList_->setMinimumWidth(220); fileList_->setMaximumWidth(320);
        fileList_->setAlternatingRowColors(true); fileList_->setSpacing(1);
        lv->addWidget(statusLbl_);
        lv->addWidget(fileList_,1);
        viewer_ = new FitsViewerWidget;
        splitter->addWidget(leftPanel);
        splitter->addWidget(viewer_);
        splitter->setStretchFactor(0,0); splitter->setStretchFactor(1,1);
        splitter->setSizes({260,900});

        // ---- bottom control row ----
        auto* ctrlRow = new QHBoxLayout;
        ctrlRow->setSpacing(6);
        auto* prevBtn = new QPushButton("◀  Prev");
        auto* nextBtn = new QPushButton("Next  ▶");
        prevBtn->setFixedWidth(110); nextBtn->setFixedWidth(110);
        prevBtn->setMinimumHeight(36); nextBtn->setMinimumHeight(36);
        // vertical separator
        auto* sep = new QFrame; sep->setFrameShape(QFrame::VLine);
        sep->setStyleSheet("QFrame{color:#1a3a5a;max-width:2px;}");
        auto* strLbl = new QLabel("Stretch:");
        strLbl->setStyleSheet("QLabel{color:#60c8f0;font-weight:bold;padding:0 4px;}");
        stretchCombo_ = new QComboBox;
        stretchCombo_->addItems({"linear","sqrt","log","asinh"});
        stretchCombo_->setFixedWidth(110); stretchCombo_->setMinimumHeight(36);
        stretchCombo_->setToolTip("s — cycle stretch mode");
        auto* delBtn = new QPushButton("Delete File");
        delBtn->setMinimumHeight(36); delBtn->setFixedWidth(160);
        delBtn->setStyleSheet("QPushButton{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #7a1818,stop:1 #5a1010);color:#ffaaaa;border:1px solid #aa3030;padding:6px 14px;border-radius:5px;font-weight:bold;}"
                              "QPushButton:hover{background:#8a2020;color:#ffcccc;}");
        delBtn->setToolTip("x / Del — permanently delete current file");
        ctrlRow->addWidget(prevBtn);
        ctrlRow->addWidget(nextBtn);
        ctrlRow->addSpacing(4);
        ctrlRow->addWidget(sep);
        ctrlRow->addSpacing(4);
        ctrlRow->addWidget(strLbl);
        ctrlRow->addWidget(stretchCombo_);
        ctrlRow->addStretch(1);
        ctrlRow->addWidget(delBtn);

        vbox->addLayout(folderRow);
        vbox->addWidget(splitter,1);
        vbox->addLayout(ctrlRow);

        // ---- connections ----
        connect(browseBtn,      &QPushButton::clicked, this, &FitsReviewerPanel::browseFolder);
        connect(openBtn,        &QPushButton::clicked, this, &FitsReviewerPanel::openFolder);
        connect(autoReviewBtn_, &QPushButton::clicked, this, &FitsReviewerPanel::autoReview);
        connect(folderEdit_,&QLineEdit::returnPressed, this, &FitsReviewerPanel::openFolder);
        connect(fileList_,  &QListWidget::currentRowChanged, this, &FitsReviewerPanel::onRowChanged);
        connect(prevBtn,  &QPushButton::clicked, this, &FitsReviewerPanel::goToPrev);
        connect(nextBtn,  &QPushButton::clicked, this, &FitsReviewerPanel::goToNext);
        connect(delBtn,   &QPushButton::clicked, this, &FitsReviewerPanel::deleteCurrentFile);
        connect(stretchCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int){ reloadCurrent(); });
        connect(viewer_, &FitsViewerWidget::nextRequested,   this, &FitsReviewerPanel::goToNext);
        connect(viewer_, &FitsViewerWidget::prevRequested,   this, &FitsReviewerPanel::goToPrev);
        connect(viewer_, &FitsViewerWidget::deleteRequested, this, &FitsReviewerPanel::deleteCurrentFile);
        connect(viewer_, &FitsViewerWidget::stretchCycleRequested, this, [this](){
            stretchCombo_->setCurrentIndex((stretchCombo_->currentIndex()+1)%stretchCombo_->count());
        });
    }

    // ---- slots ----
    void browseFolder() {
        QString d = QFileDialog::getExistingDirectory(this,"Select FITS folder",
            folderEdit_->text().isEmpty()?QDir::homePath():folderEdit_->text());
        if (!d.isEmpty()) { folderEdit_->setText(d); openFolder(); }
    }

    void openFolder() {
        QString folder = folderEdit_->text().trimmed();
        if (folder.isEmpty()||!QDir(folder).exists()) {
            QMessageBox::warning(this,"Invalid Folder","Please select a valid folder."); return;
        }
        QStringList filters={"*.fit","*.fits","*.fts","*.FIT","*.FITS","*.FTS"};
        QFileInfoList entries;
        QDirIterator it(folder, filters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); entries.append(it.fileInfo()); }
        std::sort(entries.begin(), entries.end(), [](const QFileInfo& a, const QFileInfo& b){
            return a.absoluteFilePath() < b.absoluteFilePath();
        });
        files_.clear();
        for (const auto& e : entries) files_.append(e.absoluteFilePath());
        if (files_.isEmpty()) {
            statusLbl_->setText("No FITS files found.");
            viewer_->clearAll(); fileList_->clear(); return;
        }
        statusLbl_->setText(QString("%1 FITS files — click or use arrow keys").arg(files_.size()));
        fileList_->clear();
        QDir rootDir(folder);
        for (const auto& f : files_)
            fileList_->addItem(rootDir.relativeFilePath(f));
        idx_ = 0;
        fileList_->setCurrentRow(0);
        loadAt(0);
    }

    void onRowChanged(int row) {
        if (row<0||row>=files_.size()) return;
        idx_ = row;
        loadAt(row);
    }

    void loadAt(int idx) {
        if (idx<0||idx>=files_.size()) return;
        viewer_->setError("Loading…");
        QString path = files_[idx];
        QString stretch = stretchCombo_->currentText();
        auto* w = new QFutureWatcher<FitsLoadResult>(this);
        connect(w, &QFutureWatcher<FitsLoadResult>::finished, this, [this,w,idx](){
            w->deleteLater();
            if (idx != idx_) return;
            auto r = w->result();
            if (!r.errorMsg.isEmpty()) { viewer_->setError(r.errorMsg); return; }
            QString fname = QFileInfo(files_[idx]).fileName();
            QString title = QString("%1  [%2/%3]  stretch:%4")
                .arg(fname).arg(idx+1).arg(files_.size()).arg(stretchCombo_->currentText());
            viewer_->setContent(QPixmap::fromImage(r.image), r.headerLines, r.statsLine, title);
        });
        w->setFuture(QtConcurrent::run([path,stretch](){ return loadFitsImage(path,stretch); }));
    }

    void reloadCurrent() { loadAt(idx_); }

    void goToNext() {
        if (files_.isEmpty()) return;
        idx_ = (idx_+1)%files_.size();
        fileList_->setCurrentRow(idx_);
        loadAt(idx_);
    }
    void goToPrev() {
        if (files_.isEmpty()) return;
        idx_ = (idx_-1+files_.size())%files_.size();
        fileList_->setCurrentRow(idx_);
        loadAt(idx_);
    }

    void deleteCurrentFile() {
        if (files_.isEmpty()||idx_>=files_.size()) return;
        QString path = files_[idx_], fname = QFileInfo(path).fileName();
        if (QMessageBox::question(this,"Confirm Delete",
                QString("Permanently delete?\n%1").arg(fname),
                QMessageBox::Yes|QMessageBox::No,QMessageBox::No) != QMessageBox::Yes) return;

        if (!QFile::remove(path)) {
            QMessageBox::warning(this,"Delete Failed","Could not delete:\n"+path); return;
        }
        fileList_->takeItem(idx_);
        files_.removeAt(idx_);
        statusLbl_->setText(QString("%1 FITS files").arg(files_.size()));
        if (files_.isEmpty()) { viewer_->clearAll(); return; }
        if (idx_>=files_.size()) idx_=files_.size()-1;
        fileList_->setCurrentRow(idx_);
        loadAt(idx_);
    }

    void autoReview() {
        if (files_.isEmpty()) {
            QMessageBox::information(this,"Auto Review","No FITS files loaded. Open a folder first.");
            return;
        }
        autoReviewBtn_->setEnabled(false);
        QStringList filesToProcess = files_;
        int total = filesToProcess.size();
        statusLbl_->setText(QString("Auto-reviewing 0 / %1…").arg(total));

        auto* w = new QFutureWatcher<QVector<AutoReviewItem>>(this);
        connect(w, &QFutureWatcher<QVector<AutoReviewItem>>::finished, this, [this, w, total]() {
            w->deleteLater();
            autoReviewBtn_->setEnabled(true);
            statusLbl_->setText(QString("%1 FITS files").arg(files_.size()));
            showAutoReviewResults(w->result());
        });
        w->setFuture(QtConcurrent::run([this, filesToProcess, total]() -> QVector<AutoReviewItem> {
            QVector<AutoReviewItem> results;
            for (int i = 0; i < filesToProcess.size(); i++) {
                results.append(analyzeFrame(filesToProcess[i]));
                int done = i + 1;
                QMetaObject::invokeMethod(this, [this, done, total]() {
                    statusLbl_->setText(QString("Auto-reviewing %1 / %2…").arg(done).arg(total));
                }, Qt::QueuedConnection);
            }
            return results;
        }));
    }

    void showAutoReviewResults(const QVector<AutoReviewItem>& results) {
        // Collect flagged light frames
        QVector<AutoReviewItem> bad;
        int calibCount = 0;
        for (const auto& r : results) {
            if (r.isCalib) { calibCount++; continue; }
            if (r.deleteSuggested) bad.append(r);
        }

        if (bad.isEmpty()) {
            QMessageBox::information(this, "Auto Review — All Good",
                QString("All %1 light frame%2 look good!%3")
                    .arg(results.size() - calibCount)
                    .arg(results.size() - calibCount == 1 ? "" : "s")
                    .arg(calibCount ? QString("  (%1 calibration frame%2 skipped)")
                        .arg(calibCount).arg(calibCount==1?"":"s") : ""));
            return;
        }

        QDialog dlg(this);
        dlg.setWindowTitle(QString("Auto Review — %1 issue%2 found").arg(bad.size()).arg(bad.size()==1?"":"s"));
        dlg.resize(720, 420);
        auto* vl = new QVBoxLayout(&dlg);

        auto* hdr = new QLabel(
            QString("%1 file%2 flagged. Uncheck any you want to keep, then click Delete.")
                .arg(bad.size()).arg(bad.size()==1?"":"s"));
        hdr->setStyleSheet("QLabel{color:#ffd060;font-weight:bold;font-size:10pt;padding:6px 4px;}");
        vl->addWidget(hdr);

        if (calibCount)
            vl->addWidget(new QLabel(QString("(%1 calibration frame%2 skipped automatically)")
                .arg(calibCount).arg(calibCount==1?"":"s")));

        auto* list = new QListWidget;
        list->setAlternatingRowColors(true);
        list->setSpacing(1);
        for (const auto& item : bad) {
            QString label = QString("[%1 star%2]  %3  —  %4")
                .arg(item.starCount)
                .arg(item.starCount==1?"":"s")
                .arg(QFileInfo(item.path).fileName())
                .arg(item.reason);
            auto* li = new QListWidgetItem(label, list);
            li->setCheckState(Qt::Checked);
            li->setData(Qt::UserRole, item.path);
            li->setForeground(QColor(255, 160, 100));
        }
        vl->addWidget(list, 1);

        auto* btnRow = new QHBoxLayout;
        auto* delBtn = new QPushButton(QString("Delete Checked (%1)").arg(bad.size()));
        delBtn->setStyleSheet(
            "QPushButton{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #7a1818,stop:1 #5a1010);"
            "color:#ffaaaa;border:1px solid #aa3030;border-radius:5px;padding:6px 18px;font-weight:bold;}"
            "QPushButton:hover{background:#8a2020;color:#ffcccc;}");
        auto* cancelBtn = new QPushButton("Cancel");
        btnRow->addWidget(delBtn);
        btnRow->addStretch(1);
        btnRow->addWidget(cancelBtn);
        vl->addLayout(btnRow);

        connect(list, &QListWidget::itemChanged, this, [&]() {
            int cnt = 0;
            for (int i = 0; i < list->count(); i++)
                if (list->item(i)->checkState() == Qt::Checked) cnt++;
            delBtn->setText(QString("Delete Checked (%1)").arg(cnt));
            delBtn->setEnabled(cnt > 0);
        });
        connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(delBtn,    &QPushButton::clicked, &dlg, &QDialog::accept);

        if (dlg.exec() != QDialog::Accepted) return;

        // Collect paths to delete (checked items)
        QStringList toDelete;
        for (int i = 0; i < list->count(); i++)
            if (list->item(i)->checkState() == Qt::Checked)
                toDelete.append(list->item(i)->data(Qt::UserRole).toString());
        if (toDelete.isEmpty()) return;

        // Remove in reverse index order to keep indices stable
        QList<int> indices;
        for (const auto& p : toDelete) {
            int idx = files_.indexOf(p);
            if (idx >= 0) indices.append(idx);
        }
        std::sort(indices.begin(), indices.end(), std::greater<int>());

        int deleted = 0, failed = 0;
        for (int idx : indices) {
            if (QFile::remove(files_[idx])) {
                fileList_->takeItem(idx);
                files_.removeAt(idx);
                deleted++;
            } else {
                failed++;
            }
        }

        statusLbl_->setText(QString("%1 FITS files").arg(files_.size()));
        if (files_.isEmpty()) {
            viewer_->clearAll();
        } else {
            idx_ = std::min(idx_, (int)files_.size() - 1);
            fileList_->setCurrentRow(idx_);
            loadAt(idx_);
        }

        QString msg = QString("Deleted %1 file%2.").arg(deleted).arg(deleted==1?"":"s");
        if (failed) msg += QString("\n%1 file%2 could not be deleted.").arg(failed).arg(failed==1?"":"s");
        QMessageBox::information(this, "Auto Review — Done", msg);
    }

    QLineEdit*    folderEdit_    = nullptr;
    QListWidget*  fileList_      = nullptr;
    QLabel*       statusLbl_     = nullptr;
    FitsViewerWidget* viewer_    = nullptr;
    QComboBox*    stretchCombo_  = nullptr;
    QPushButton*  autoReviewBtn_ = nullptr;
    QStringList   files_;
    int           idx_           = 0;
};

// ============================================================
// Tools — Sort FITS (C++ using cfitsio, replaces sort_multiday.sh)
// ============================================================
struct SortJob {
    QString source, destination, rejectFile;
    bool dryRun = false;
    bool splitByExp = false;
};
struct SortProgress { QString message; bool isError = false; };

struct FitsBasicMeta { QString obj, filter; float exptime = -1.0f; };

static FitsBasicMeta readFitsObjFilter(const QString& path)
{
    fitsfile* fp = nullptr; int st = 0;
    fits_open_file(&fp, path.toLocal8Bit().constData(), READONLY, &st);
    if (st) return {"_NoObject_","_NoFilter_",-1.0f};
    auto rk = [&](const char* k) -> QString {
        char v[FLEN_VALUE]=""; int s=0;
        fits_read_key(fp, TSTRING, k, v, nullptr, &s);
        return s==0 ? QString(v).trimmed() : QString();
    };
    auto rd = [&](const char* k) -> float {
        double v=0; int s=0;
        fits_read_key(fp, TDOUBLE, k, &v, nullptr, &s);
        return s==0 ? (float)v : -1.0f;
    };
    FitsBasicMeta m;
    m.obj    = rk("OBJECT");  if (m.obj.isEmpty())    m.obj    = "_NoObject_";
    m.filter = rk("FILTER");  if (m.filter.isEmpty()) m.filter = "_NoFilter_";
    m.exptime = rd("EXPTIME"); if (m.exptime < 0) m.exptime = rd("EXPOSURE");
    fits_close_file(fp, &st);
    return m;
}

// Format exposure time as a folder-safe string e.g. 300.0 -> "300s", 1.5 -> "1.5s"
static QString expFolder(float exptime) {
    if (exptime <= 0) return "_NoExp_";
    float rounded = std::round(exptime * 10.0f) / 10.0f;
    if (std::fmod(rounded, 1.0f) < 0.05f)
        return QString("%1s").arg((int)std::round(rounded));
    return QString("%1s").arg(rounded, 0, 'f', 1);
}

static QString safeName(const QString& n) {
    QString s = n;
    // Replace characters unsafe in folder names
    for (QChar c : {'/', ':', '*', '?', '"', '<', '>', '|', '\\'})
        s.replace(c, '_');
    s = s.trimmed();
    return s.isEmpty() ? "_unknown_" : s;
}

static QStringList kFitsExts() {
    return {"*.fit","*.fits","*.fts","*.FIT","*.FITS","*.FTS"};
}

struct CalibMeta {
    float gain    = -1.0f;
    float exptime = -1.0f;
    float ccdTemp = -999.0f;
    int   naxis1  = 0;   // image width  (NAXIS1)
    int   naxis2  = 0;   // image height (NAXIS2)
    QString filter, frameType, instrume, path;
};

static CalibMeta readCalibMeta(const QString& path) {
    CalibMeta m; m.path = path;
    fitsfile* fp = nullptr; int st = 0;
    fits_open_file(&fp, path.toLocal8Bit().constData(), READONLY, &st);
    if (st) return m;
    auto rk = [&](const char* k) -> QString {
        char v[FLEN_VALUE]=""; int s=0;
        fits_read_key(fp, TSTRING, k, v, nullptr, &s);
        return s==0 ? QString(v).trimmed() : QString();
    };
    auto rd = [&](const char* k) -> float {
        double v=0; int s=0;
        fits_read_key(fp, TDOUBLE, k, &v, nullptr, &s);
        return s==0 ? (float)v : -1.0f;
    };
    m.gain    = rd("GAIN");    if(m.gain<0)    m.gain    = rd("EGAIN");
    m.exptime = rd("EXPTIME"); if(m.exptime<0) m.exptime = rd("EXPOSURE");
    m.filter    = rk("FILTER");
    m.frameType = rk("IMAGETYP");
    m.instrume  = rk("INSTRUME");
    m.ccdTemp   = rd("CCD-TEMP"); if(m.ccdTemp<-900) m.ccdTemp = rd("SET-TEMP");
    // Image dimensions — critical for matching calibration frames to lights
    { int nax=0, s2=0; fits_get_img_dim(fp,&nax,&s2);
      if(nax>=2){ long sz[2]={0,0}; fits_get_img_size(fp,2,sz,&s2);
                  m.naxis1=(int)sz[0]; m.naxis2=(int)sz[1]; } }
    fits_close_file(fp, &st);
    return m;
}

// Match gain within 15% or 1 ADU tolerance; -1 = unknown = always match
static bool gainMatch(float a, float b) {
    if (a<0||b<0) return true;
    return std::abs(a-b) <= std::max(1.0f, std::min(a,b)*0.15f);
}
// Match exposure within 10% or 0.5s tolerance
static bool expMatch(float a, float b) {
    if (a<0||b<0) return true;
    return std::abs(a-b) <= std::max(0.5f, std::min(a,b)*0.10f);
}
// Sensor dimensions must match exactly (0 = unknown, accept any)
static bool dimMatch(int w1, int h1, int w2, int h2) {
    if (w1==0||w2==0) return true;
    return w1==w2 && h1==h2;
}

class SortFitsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SortFitsPanel(QWidget* parent=nullptr) : QWidget(parent) {
        auto* vbox = new QVBoxLayout(this);
        vbox->setSpacing(8); vbox->setContentsMargins(8,8,8,8);

        auto row = [&](const QString& lbl, QLineEdit*& edit, QPushButton*& btn){
            auto* h = new QHBoxLayout;
            auto* l = new QLabel(lbl); l->setFixedWidth(110);
            edit = new QLineEdit; edit->setMinimumHeight(30);
            btn  = new QPushButton("Browse…"); btn->setFixedWidth(90); btn->setMinimumHeight(30);
            h->addWidget(l); h->addWidget(edit,1); h->addWidget(btn);
            vbox->addLayout(h);
        };

        QPushButton *bSrc,*bDst,*bRej;
        row("Source folder:", srcEdit_, bSrc);
        row("Destination:",   dstEdit_, bDst);
        row("Reject file (opt):", rejEdit_, bRej);
        bRej->setText("Pick file…");

        auto* optRow = new QHBoxLayout;
        dryChk_ = new QCheckBox("Dry run (simulate, no copy)");
        dryChk_->setToolTip("Shows what would happen without copying any files");
        splitExpChk_ = new QCheckBox("Split by exposure time");
        splitExpChk_->setToolTip("Create subfolders per exposure time: Object/Filter/300s/");
        optRow->addWidget(dryChk_); optRow->addWidget(splitExpChk_); optRow->addStretch(1);
        auto* runBtn = new QPushButton("Sort FITS by Object / Filter");
        runBtn->setMinimumHeight(36);
        runBtn->setStyleSheet("QPushButton{font-weight:bold;font-size:10pt;}");
        optRow->addWidget(runBtn);
        vbox->addLayout(optRow);

        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setFont(QFont("Monospace",9));
        log_->setStyleSheet("QPlainTextEdit{background:#060e18;color:#b8d0e8;"
                            "border:1px solid #1a3650;border-radius:5px;}");
        vbox->addWidget(log_,1);

        connect(bSrc, &QPushButton::clicked, this, [this](){
            QString d=QFileDialog::getExistingDirectory(this,"Source folder",srcEdit_->text());
            if(!d.isEmpty())srcEdit_->setText(d); });
        connect(bDst, &QPushButton::clicked, this, [this](){
            QString d=QFileDialog::getExistingDirectory(this,"Destination folder",dstEdit_->text());
            if(!d.isEmpty())dstEdit_->setText(d); });
        connect(bRej, &QPushButton::clicked, this, [this](){
            QString f=QFileDialog::getOpenFileName(this,"Reject list",rejEdit_->text(),"Text (*.txt);;All (*)");
            if(!f.isEmpty())rejEdit_->setText(f); });
        connect(runBtn, &QPushButton::clicked, this, &SortFitsPanel::runSort);
    }

private slots:
    void runSort() {
        SortJob job;
        job.source      = srcEdit_->text().trimmed();
        job.destination = dstEdit_->text().trimmed();
        job.rejectFile  = rejEdit_->text().trimmed();
        job.dryRun      = dryChk_->isChecked();
        job.splitByExp  = splitExpChk_->isChecked();

        if (job.source.isEmpty()||!QDir(job.source).exists()) {
            QMessageBox::warning(this,"Source","Select a valid source folder."); return;
        }
        if (job.destination.isEmpty()) {
            QMessageBox::warning(this,"Destination","Select a destination folder."); return;
        }
        log_->clear();
        log_->appendPlainText(job.dryRun ? "[DRY RUN] No files will be copied.\n" : "Starting sort…\n");

        QSet<QString> rejects;
        if (!job.rejectFile.isEmpty() && QFile::exists(job.rejectFile)) {
            QFile rf(job.rejectFile); if(rf.open(QIODevice::ReadOnly)){
                QTextStream ts(&rf);
                while(!ts.atEnd()){ QString l=ts.readLine().trimmed(); if(!l.isEmpty())rejects.insert(QFileInfo(l).fileName()); }
            }
            log_->appendPlainText(QString("Loaded %1 rejected filenames.\n").arg(rejects.size()));
        }

        // Collect FITS files
        QDir srcDir(job.source);
        QStringList filters={"*.fit","*.fits","*.fts","*.FIT","*.FITS","*.FTS"};
        QFileInfoList allFiles;
        // recursive
        QDirIterator it(job.source, filters, QDir::Files, QDirIterator::Subdirectories);
        while(it.hasNext()){ it.next(); allFiles.append(it.fileInfo()); }
        std::sort(allFiles.begin(),allFiles.end(),[](const QFileInfo&a,const QFileInfo&b){return a.absoluteFilePath()<b.absoluteFilePath();});

        log_->appendPlainText(QString("Found %1 FITS file(s).\n").arg(allFiles.size()));
        if(allFiles.isEmpty()) return;

        // CSV log
        QDir().mkpath(job.destination);
        QString csvPath = job.destination + "/sort_log.csv";
        QFile csvFile(csvPath);
        bool csvOk = !job.dryRun && csvFile.open(QIODevice::WriteOnly|QIODevice::Text);
        QTextStream csv; if(csvOk) csv.setDevice(&csvFile);
        if(csvOk) csv << "SourceFile,FileName,OBJECT,FILTER,Destination,Status\n";

        int copied=0, rejected=0, errors=0;

        auto logLine = [this](const QString& s){ log_->appendPlainText(s); QApplication::processEvents(); };

        for(const QFileInfo& fi : allFiles){
            const QString path = fi.absoluteFilePath();
            const QString fname = fi.fileName();
            logLine(QString("Processing: %1").arg(fname));

            auto meta = readFitsObjFilter(path);
            QString safeObj=safeName(meta.obj), safeFlt=safeName(meta.filter);
            QString expSub = job.splitByExp ? "/" + expFolder(meta.exptime) : "";

            bool isReject = rejects.contains(fname);
            QString destDir = isReject
                ? job.destination+"/rejected/"+safeObj+"/"+safeFlt+expSub
                : job.destination+"/"+safeObj+"/"+safeFlt+expSub;
            QString destFile = destDir+"/"+fname;

            // handle duplicate
            if(QFile::exists(destFile)){
                QString stem=fi.completeBaseName(), ext=fi.suffix();
                destFile=destDir+"/"+stem+"_"+QString::number(QDateTime::currentMSecsSinceEpoch())+"."+ext;
            }

            QString status = isReject?"Rejected":"Copied";
            logLine(QString("  -> [%1] %2/%3%4/").arg(status,safeObj,safeFlt,expSub));

            if(!job.dryRun){
                QDir().mkpath(destDir);
                if(!QFile::copy(path,destFile)){
                    logLine(QString("  ERROR: copy failed for %1").arg(fname));
                    errors++; status="Error";
                }
            }

            if(isReject)rejected++;else copied++;
            if(csvOk) csv<<"\""<<path<<"\"" <<","<< "\""<<fname<<"\"" <<","<< "\""<<meta.obj<<"\"" <<","<< "\""<<meta.filter<<"\"" <<","<< "\""<<destFile<<"\"" <<","<< "\""<<status<<"\"\n";
        }

        if(csvOk){ csvFile.close(); logLine("Log: "+csvPath); }
        logLine(QString("\n--- Summary ---\nCopied: %1\nRejected: %2\nErrors: %3").arg(copied).arg(rejected).arg(errors));
        if(job.dryRun) logLine("[DRY RUN complete — no files were actually copied]");
    }

private:
    QLineEdit    *srcEdit_=nullptr, *dstEdit_=nullptr, *rejEdit_=nullptr;
    QCheckBox    *dryChk_=nullptr, *splitExpChk_=nullptr;
    QPlainTextEdit *log_=nullptr;
};

// ============================================================
// Tools — Siril Stack Panel
// ============================================================
class SirilStackPanel : public QWidget {
    Q_OBJECT

    // Holds everything needed to stack one group — built during runStack() prep phase
    struct StackGroup {
        QString groupDir;    // session/GroupName  (calib subfolders + process/ go here)
        QString lightsPath;  // original sorted lights folder — Siril reads directly, no copy
        float   darkScale = 1.0f;
        bool    hasCalib  = false;
    };

public:
    explicit SirilStackPanel(QWidget* parent=nullptr) : QWidget(parent) {
        auto* vbox = new QVBoxLayout(this);
        vbox->setSpacing(8); vbox->setContentsMargins(8,8,8,8);

        auto makeRow = [&](const QString& lbl, QLineEdit*& edit, QPushButton*& btn, QVBoxLayout* target, int lw=150){
            auto* h = new QHBoxLayout;
            auto* l = new QLabel(lbl); l->setFixedWidth(lw);
            edit = new QLineEdit; edit->setMinimumHeight(30);
            btn  = new QPushButton("Browse…"); btn->setFixedWidth(90); btn->setMinimumHeight(30);
            h->addWidget(l); h->addWidget(edit,1); h->addWidget(btn);
            target->addLayout(h);
        };

        QPushButton *bLights, *bSess;
        makeRow("Sorted lights folder:", lightsEdit_, bLights, vbox);
        makeRow("Session output folder:", sessionEdit_, bSess, vbox);

        // Calibration group
        auto* calibGrp = new QGroupBox("Calibration frames  (optional — leave blank to skip)");
        calibGrp->setStyleSheet("QGroupBox{font-weight:bold;color:#60c8f0;border:1px solid #1a4a6a;"
                                "border-radius:6px;margin-top:8px;padding-top:6px;}"
                                "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;}");
        auto* cg = new QVBoxLayout(calibGrp);
        cg->setSpacing(6); cg->setContentsMargins(8,10,8,8);

        auto makeCalibRow = [&](const QString& lbl, QLineEdit*& edit, QPushButton*& btn){
            auto* h = new QHBoxLayout;
            auto* l = new QLabel(lbl); l->setFixedWidth(110);
            edit = new QLineEdit; edit->setMinimumHeight(28);
            edit->setPlaceholderText("optional — leave blank to skip");
            btn  = new QPushButton("Browse…"); btn->setFixedWidth(90); btn->setMinimumHeight(28);
            auto* clr = new QPushButton("✕"); clr->setFixedWidth(28); clr->setMinimumHeight(28);
            clr->setToolTip("Clear");
            clr->setStyleSheet("QPushButton{padding:0;font-size:9pt;background:#0c1828;color:#aa4040;"
                               "border:1px solid #2a1a1a;border-radius:3px;}"
                               "QPushButton:hover{background:#1a0808;color:#ff6060;}");
            QLineEdit* ep = edit;
            connect(clr, &QPushButton::clicked, this, [ep](){ ep->clear(); });
            h->addWidget(l); h->addWidget(edit,1); h->addWidget(btn); h->addWidget(clr);
            cg->addLayout(h);
        };

        QPushButton *bBias, *bDark, *bFlat;
        makeCalibRow("Biases folder:", biasEdit_, bBias);
        makeCalibRow("Darks folder:",  darkEdit_, bDark);
        makeCalibRow("Flats folder:",  flatEdit_, bFlat);
        vbox->addWidget(calibGrp);

        // Stack options
        auto* h3 = new QHBoxLayout;
        auto* lm = new QLabel("Stack method:"); lm->setFixedWidth(105);
        methodCombo_ = new QComboBox;
        methodCombo_->addItems({"rej","average","median","winsorized","linear"});
        methodCombo_->setMinimumHeight(30); methodCombo_->setFixedWidth(130);
        auto* ln = new QLabel("Norm:"); ln->setFixedWidth(45);
        normCombo_ = new QComboBox;
        normCombo_->addItems({"addscale","mulscale","addoffset","no"});
        normCombo_->setMinimumHeight(30); normCombo_->setFixedWidth(130);
        auto* lsig = new QLabel("Sigma lo/hi:"); lsig->setFixedWidth(90);
        lsig->setStyleSheet("QLabel{color:#60c8f0;font-weight:bold;padding:0 4px;}");
        sigLowEdit_  = new QLineEdit("3"); sigLowEdit_->setFixedWidth(42); sigLowEdit_->setMinimumHeight(30);
        sigHighEdit_ = new QLineEdit("3"); sigHighEdit_->setFixedWidth(42); sigHighEdit_->setMinimumHeight(30);
        sigLowEdit_->setToolTip("Sigma low for rejection stacking (default 3)");
        sigHighEdit_->setToolTip("Sigma high for rejection stacking (default 3)");
        auto* slash = new QLabel("/"); slash->setFixedWidth(10);
        h3->addWidget(lm); h3->addWidget(methodCombo_);
        h3->addSpacing(10); h3->addWidget(ln); h3->addWidget(normCombo_);
        h3->addSpacing(10); h3->addWidget(lsig); h3->addWidget(sigLowEdit_);
        h3->addWidget(slash); h3->addWidget(sigHighEdit_);
        h3->addStretch(1);
        vbox->addLayout(h3);

        // Action buttons — single Stack button, no separate Prepare step
        auto* h4 = new QHBoxLayout; h4->setSpacing(8);
        auto* runBtn = new QPushButton("▶▶  Stack with Siril");
        runBtn->setMinimumHeight(38); runBtn->setMinimumWidth(210);
        runBtn->setToolTip("Match calibration frames, generate Siril scripts and run stacking in one step.\n"
                           "Light frames are read directly — no intermediate copy needed.");
        runBtn->setStyleSheet("QPushButton{font-weight:bold;font-size:10pt;background:"
                              "qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #0e5a28,stop:1 #093d1a);"
                              "color:#80ffb0;border:1px solid #1a8040;}"
                              "QPushButton:hover{background:#147830;color:#b0ffd0;}");
        killBtn_ = new QPushButton("Stop");
        killBtn_->setMinimumHeight(38); killBtn_->setFixedWidth(80);
        killBtn_->setEnabled(false);
        killBtn_->setStyleSheet("QPushButton{background:#7a1818;color:#ffaaaa;border:1px solid #aa3030;border-radius:5px;}"
                                "QPushButton:hover{background:#8a2020;}");
        h4->addStretch(1); h4->addWidget(runBtn); h4->addWidget(killBtn_);
        vbox->addLayout(h4);

        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setFont(QFont("Monospace",9));
        log_->setStyleSheet("QPlainTextEdit{background:#060e18;color:#b8d0e8;"
                            "border:1px solid #1a3650;border-radius:5px;}");
        vbox->addWidget(log_,1);

        // Browse connections
        auto browse = [this](QLineEdit* e, const QString& title){
            QString d=QFileDialog::getExistingDirectory(this,title,e->text());
            if(!d.isEmpty()) e->setText(d);
        };
        connect(bLights, &QPushButton::clicked, this, [this,browse](){ browse(lightsEdit_,"Sorted lights folder"); });
        connect(bSess,   &QPushButton::clicked, this, [this,browse](){ browse(sessionEdit_,"Session output folder"); });
        connect(bBias,   &QPushButton::clicked, this, [this,browse](){ browse(biasEdit_,"Biases calibration folder"); });
        connect(bDark,   &QPushButton::clicked, this, [this,browse](){ browse(darkEdit_,"Darks calibration folder"); });
        connect(bFlat,   &QPushButton::clicked, this, [this,browse](){ browse(flatEdit_,"Flats calibration folder"); });
        connect(runBtn,  &QPushButton::clicked, this, &SirilStackPanel::runStack);
        connect(killBtn_,&QPushButton::clicked, this, &SirilStackPanel::stopStack);
    }

private slots:
    void runStack() {
        QString lightsRoot = lightsEdit_->text().trimmed();
        QString session    = sessionEdit_->text().trimmed();
        if (lightsRoot.isEmpty()||!QDir(lightsRoot).exists()) {
            QMessageBox::warning(this,"Lights","Select a valid sorted lights folder."); return;
        }
        if (session.isEmpty()) {
            QMessageBox::warning(this,"Session","Select a session output folder."); return;
        }

        QString sirilBin;
        for (const QString c : {"siril-cli","siril"}) {
            QProcess p; p.start("which",{c}); p.waitForFinished(3000);
            if (p.exitCode()==0) { sirilBin=c; break; }
        }
        if (sirilBin.isEmpty()) {
            QMessageBox::critical(this,"Siril Not Found",
                "siril-cli not found in PATH.\nInstall from https://siril.org"); return;
        }

        QDir().mkpath(session);
        log_->clear();
        auto logLine = [this](const QString& s){
            log_->appendPlainText(s); QApplication::processEvents();
        };
        logLine("=== Stack with Siril ===");
        logLine("Lights: " + lightsRoot);
        logLine("Session: " + session);
        logLine("Siril: " + sirilBin + "\n");

        // Load calibration metadata
        auto loadCalibDir = [&](const QString& dir) -> QVector<CalibMeta> {
            QVector<CalibMeta> v;
            if (dir.isEmpty()||!QDir(dir).exists()) return v;
            QDirIterator it(dir, kFitsExts(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) { it.next(); v.append(readCalibMeta(it.filePath())); }
            logLine(QString("  Loaded %1 calib frames from: %2").arg(v.size()).arg(QFileInfo(dir).fileName()));
            return v;
        };
        QVector<CalibMeta> biases = loadCalibDir(biasEdit_->text().trimmed());
        QVector<CalibMeta> darks  = loadCalibDir(darkEdit_->text().trimmed());
        QVector<CalibMeta> flats  = loadCalibDir(flatEdit_->text().trimmed());

        // Discover light groups: Object/Filter/*.fit  OR  Filter/*.fit
        struct LightGroup { QString object, filter, srcPath; QFileInfoList files; };
        QVector<LightGroup> groups;
        QDir rootDir(lightsRoot);
        for (const auto& l1 : rootDir.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
            QDir l1dir(l1.absoluteFilePath());
            auto directFits = l1dir.entryInfoList(kFitsExts(), QDir::Files);
            if (!directFits.isEmpty()) {
                groups.append({"_all_", l1.fileName(), l1.absoluteFilePath(), directFits});
            } else {
                for (const auto& l2 : l1dir.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) {
                    auto fits = QDir(l2.absoluteFilePath()).entryInfoList(kFitsExts(), QDir::Files);
                    if (!fits.isEmpty())
                        groups.append({l1.fileName(), l2.fileName(), l2.absoluteFilePath(), fits});
                }
            }
        }
        if (groups.isEmpty()) {
            logLine("ERROR: No FITS light groups found.\n"
                    "Expected: SortedRoot/Object/Filter/*.fit  or  SortedRoot/Filter/*.fit");
            return;
        }
        logLine(QString("Found %1 light group(s):\n").arg(groups.size()));

        stackGroups_.clear();
        for (const auto& g : groups) {
            QString groupName = safeName(g.object=="_all_" ? g.filter : g.object+"_"+g.filter);
            logLine(QString("--- %1 (%2 files) ---").arg(groupName).arg(g.files.size()));

            // Pick dominant exposure time when mixed — cache metadata to avoid redundant FITS I/O
            QMap<int,QFileInfoList> byExp;
            QHash<QString,CalibMeta> metaCache;
            for (const auto& fi : g.files) {
                CalibMeta m = readCalibMeta(fi.absoluteFilePath());
                metaCache[fi.absoluteFilePath()] = m;
                int key = (m.exptime>0) ? (int)std::round(m.exptime*10.0f) : 0;
                byExp[key].append(fi);
            }
            QFileInfoList chosenFiles;
            if (byExp.size()==1) {
                chosenFiles = byExp.first();
            } else {
                logLine("  Multiple exposure times:");
                for (auto it=byExp.begin(); it!=byExp.end(); ++it)
                    logLine(QString("    %1s -> %2 files").arg(it.key()/10.0f,0,'f',1).arg(it.value().size()));
                auto bestIt = std::max_element(byExp.begin(), byExp.end(),
                    [](const QFileInfoList& a, const QFileInfoList& b){ return a.size() < b.size(); });
                chosenFiles = bestIt.value();
                logLine(QString("  -> Using dominant exposure %1s (%2 files)")
                    .arg(bestIt.key()/10.0f,0,'f',1).arg(bestIt.value().size()));
            }

            CalibMeta rep = metaCache.value(chosenFiles.first().absoluteFilePath());
            QString groupDir = session + "/" + groupName;
            QDir().mkpath(groupDir + "/process");

            StackGroup sg;
            sg.groupDir   = groupDir;
            sg.lightsPath = g.srcPath;   // Siril reads lights directly — no copy

            // Match & copy biases
            if (!biases.isEmpty()) {
                QVector<CalibMeta> m;
                for (const auto& b:biases)
                    if (gainMatch(b.gain,rep.gain) && dimMatch(b.naxis1,b.naxis2,rep.naxis1,rep.naxis2))
                        m.append(b);
                if (!m.isEmpty()) {
                    QDir().mkpath(groupDir+"/biases");
                    for (const auto& b:m) QFile::copy(b.path, groupDir+"/biases/"+QFileInfo(b.path).fileName());
                    logLine(QString("  Biases: matched %1/%2").arg(m.size()).arg(biases.size()));
                    sg.hasCalib = true;
                } else {
                    logLine(QString("  Biases: no gain match (light gain=%1)").arg(rep.gain,0,'f',1));
                }
            }

            // Match & copy darks (relax exposure if needed) — single pass
            if (!darks.isEmpty()) {
                QVector<CalibMeta> m, mRelaxed;
                for (const auto& d:darks) {
                    if (gainMatch(d.gain,rep.gain) && dimMatch(d.naxis1,d.naxis2,rep.naxis1,rep.naxis2)) {
                        if (expMatch(d.exptime,rep.exptime)) m.append(d);
                        else mRelaxed.append(d);
                    }
                }
                bool relaxed = m.isEmpty();
                if (relaxed) m = std::move(mRelaxed);
                if (!m.isEmpty()) {
                    QDir().mkpath(groupDir+"/darks");
                    for (const auto& d:m) QFile::copy(d.path, groupDir+"/darks/"+QFileInfo(d.path).fileName());
                    if (relaxed) {
                        float darkExpAvg = 0.0f;
                        for (const auto& d:m) darkExpAvg += (d.exptime>0 ? d.exptime : 0.0f);
                        darkExpAvg /= m.size();
                        float lightExp = rep.exptime>0 ? rep.exptime : 0.0f;
                        float ratio = (darkExpAvg>0 && lightExp>0) ? lightExp/darkExpAvg : 1.0f;
                        logLine(QString("  *** WARNING: Darks exposure mismatch — lights=%1s darks=%2s dark_mul=%3")
                            .arg(lightExp,0,'f',1).arg(darkExpAvg,0,'f',1).arg(ratio,0,'f',4));
                        if (ratio<0.4f||ratio>2.5f)
                            logLine("  *** STRONG WARNING: ratio extreme (>2.5x or <0.4x) — results may be poor!");
                        sg.darkScale = ratio;
                    } else {
                        logLine(QString("  Darks: matched %1/%2 (exp=%3s, gain=%4)")
                            .arg(m.size()).arg(darks.size()).arg(rep.exptime,0,'f',1).arg(rep.gain,0,'f',1));
                    }
                    sg.hasCalib = true;
                } else {
                    logLine("  Darks: no match found");
                }
            }

            // Match & copy flats (filter+gain; fall back to gain-only) — single pass
            if (!flats.isEmpty()) {
                QVector<CalibMeta> m, mRelaxed;
                for (const auto& f:flats) {
                    if (gainMatch(f.gain,rep.gain) && dimMatch(f.naxis1,f.naxis2,rep.naxis1,rep.naxis2)) {
                        bool fltOk = f.filter.isEmpty()||g.filter.isEmpty()||
                                     f.filter.compare(g.filter,Qt::CaseInsensitive)==0;
                        if (fltOk) m.append(f);
                        else mRelaxed.append(f);
                    }
                }
                if (m.isEmpty()) {
                    m = std::move(mRelaxed);
                    if (!m.isEmpty()) logLine("  Flats: filter mismatch, using gain-matched flats");
                }
                if (!m.isEmpty()) {
                    QDir().mkpath(groupDir+"/flats");
                    for (const auto& f:m) QFile::copy(f.path, groupDir+"/flats/"+QFileInfo(f.path).fileName());
                    logLine(QString("  Flats: matched %1/%2").arg(m.size()).arg(flats.size()));
                    sg.hasCalib = true;
                } else {
                    logLine("  Flats: no match found");
                }
            }

            stackGroups_.append(sg);
            logLine("");
        }

        sessionDir_ = session; sirilBin_ = sirilBin; filterIdx_ = 0;
        processNextFilter();
    }

    void processNextFilter() {
        if (filterIdx_ >= stackGroups_.size()) {
            log_->appendPlainText("\n=== All groups processed. ===");
            killBtn_->setEnabled(false); return;
        }
        const StackGroup& sg = stackGroups_[filterIdx_];
        QString groupDir  = sg.groupDir;
        QString groupName = QFileInfo(groupDir).fileName();
        log_->appendPlainText(QString("\n--- Stacking: %1 ---").arg(groupName));

        auto hasFits = [](const QString& dir) -> bool {
            return QDir(dir).exists() &&
                   !QDir(dir).entryInfoList(kFitsExts(),QDir::Files).isEmpty();
        };
        bool hasBias  = hasFits(groupDir+"/biases");
        bool hasDark  = hasFits(groupDir+"/darks");
        bool hasFlatF = hasFits(groupDir+"/flats");
        bool hasCalib = hasBias||hasDark||hasFlatF;

        QString method  = methodCombo_->currentText();
        QString norm    = normCombo_->currentText();
        QString sigLo   = sigLowEdit_->text().trimmed().isEmpty()  ? "3" : sigLowEdit_->text().trimmed();
        QString sigHi   = sigHighEdit_->text().trimmed().isEmpty() ? "3" : sigHighEdit_->text().trimmed();
        QString rejArgs = (method == "rej") ? QString(" %1 %2").arg(sigLo, sigHi) : "";
        QDir().mkpath(groupDir+"/process");

        QString script = groupDir+"/_stack.ssf";
        {
            QFile sf(script); if (!sf.open(QIODevice::WriteOnly|QIODevice::Text)) return;
            QTextStream ts(&sf);
            ts << "requires 1.0.0\n\n";
            if (hasCalib) {
                if (hasBias) {
                    ts << "cd " << groupDir << "/biases\n"
                       << "convert bias -out=../process\n"
                       << "cd ../process\n"
                       << "stack bias_ rej 3 3 -norm=no\n\n";
                }
                if (hasDark) {
                    ts << "cd " << groupDir << "/darks\n"
                       << "convert dark -out=../process\n"
                       << "cd ../process\n"
                       << "stack dark_ rej 3 3 -norm=no\n\n";
                }
                if (hasFlatF) {
                    ts << "cd " << groupDir << "/flats\n"
                       << "convert flat -out=../process\n"
                       << "cd ../process\n"
                       << "stack flat_ rej 3 3 -norm=mulscale\n\n";
                }
                // Read lights directly from original sorted folder — no copy needed
                ts << "cd " << sg.lightsPath << "\n"
                   << "convert light -out=" << groupDir << "/process\n"
                   << "cd " << groupDir << "/process\n";
                QString calCmd = "calibrate light_";
                if (hasBias) calCmd += " -bias=bias_stacked";
                if (hasDark) {
                    calCmd += " -dark=dark_stacked";
                    if (std::abs(sg.darkScale - 1.0f) > 0.001f)
                        calCmd += QString(" -dark_mul=%1").arg(sg.darkScale, 0, 'f', 4);
                }
                if (hasFlatF) calCmd += " -flat=flat_stacked";
                if (hasDark||hasBias) calCmd += " -cc=dark";
                ts << calCmd << "\n"
                   << "register pp_light_ -2pass\n"
                   << "stack r_pp_light_" << method << rejArgs << " -norm=" << norm << "\n";
            } else {
                // No calibration — read lights directly from original sorted folder
                ts << "cd " << sg.lightsPath << "\n"
                   << "convert light -out=" << groupDir << "/process\n"
                   << "cd " << groupDir << "/process\n"
                   << "register light_ -2pass\n"
                   << "stack r_light_" << method << rejArgs << " -norm=" << norm << "\n";
            }
        }
        log_->appendPlainText(QString("  Lights: %1").arg(sg.lightsPath));
        log_->appendPlainText(QString("  Calibration: bias=%1 dark=%2 flat=%3")
            .arg(hasBias?"yes":"no").arg(hasDark?"yes":"no").arg(hasFlatF?"yes":"no"));

        proc_ = new QProcess(this);
        proc_->setWorkingDirectory(groupDir);
        connect(proc_,&QProcess::readyReadStandardOutput,this,[this](){
            log_->appendPlainText(proc_->readAllStandardOutput()); });
        connect(proc_,&QProcess::readyReadStandardError,this,[this](){
            log_->appendPlainText(proc_->readAllStandardError()); });
        connect(proc_,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this,[this,groupDir,groupName,hasCalib](int code,QProcess::ExitStatus){
            proc_->deleteLater(); proc_=nullptr;
            QStringList candidates;
            if (hasCalib) candidates<<groupDir+"/process/r_pp_light__stacked.fit"
                                    <<groupDir+"/process/r_pp_light__stacked.fits"
                                    <<groupDir+"/process/r_pp_light_stacked.fit"
                                    <<groupDir+"/process/r_pp_light_stacked.fits";
            candidates<<groupDir+"/process/r_light__stacked.fit"
                      <<groupDir+"/process/r_light__stacked.fits"
                      <<groupDir+"/process/r_light_stacked.fit"
                      <<groupDir+"/process/r_light_stacked.fits";
            QString found;
            for (const auto& c:candidates) if(QFile::exists(c)){found=c;break;}
            if (code==0&&!found.isEmpty()) {
                QString out=sessionDir_+"/"+groupName+"_stacked.fit";
                QFile::copy(found,out);
                log_->appendPlainText("  -> Result: "+out);
            } else {
                log_->appendPlainText(QString("  WARNING: siril exited %1").arg(code));
                log_->appendPlainText("  Script: "+groupDir+"/_stack.ssf");
            }
            filterIdx_++; processNextFilter();
        });
        QStringList args;
        if (sirilBin_=="siril-cli") args<<"-s"<<script;
        else                         args<<"--cli"<<"-s"<<script;
        proc_->start(sirilBin_,args);
        killBtn_->setEnabled(true);
        log_->appendPlainText("  Running siril…");
    }

    void stopStack() {
        if (proc_&&proc_->state()!=QProcess::NotRunning) {
            proc_->kill(); log_->appendPlainText("\n[Stopped by user]");
        }
        killBtn_->setEnabled(false);
    }

private:
    QLineEdit      *lightsEdit_=nullptr, *sessionEdit_=nullptr;
    QLineEdit      *biasEdit_=nullptr,   *darkEdit_=nullptr, *flatEdit_=nullptr;
    QComboBox      *methodCombo_=nullptr, *normCombo_=nullptr;
    QLineEdit      *sigLowEdit_=nullptr, *sigHighEdit_=nullptr;
    QPushButton    *killBtn_=nullptr;
    QPlainTextEdit *log_=nullptr;
    QProcess       *proc_=nullptr;
    QVector<StackGroup> stackGroups_;
    QString         sessionDir_, sirilBin_;
    int             filterIdx_=0;
};

// ============================================================
// ArrangeFitsPanel — scan a messy folder, arrange into
//   arrange_<X>/Target/Filter/lights/  ready for stacking
//   with optional calibration frame matching
// ============================================================
class ArrangeFitsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ArrangeFitsPanel(QWidget* parent=nullptr) : QWidget(parent) {
        auto* vbox = new QVBoxLayout(this);
        vbox->setSpacing(8); vbox->setContentsMargins(8,8,8,8);

        // --- Input folder row ---
        {
            auto* h = new QHBoxLayout;
            auto* lbl = new QLabel("Input folder (messy):");
            lbl->setFixedWidth(160);
            inputEdit_ = new QLineEdit;
            inputEdit_->setMinimumHeight(30);
            inputEdit_->setPlaceholderText("Select the disorganised FITS folder…");
            auto* btn = new QPushButton("Browse…");
            btn->setFixedWidth(90); btn->setMinimumHeight(30);
            h->addWidget(lbl); h->addWidget(inputEdit_,1); h->addWidget(btn);
            vbox->addLayout(h);
            connect(btn, &QPushButton::clicked, this, [this](){
                QString d = QFileDialog::getExistingDirectory(this, "Select messy FITS folder",
                                                              inputEdit_->text());
                if (!d.isEmpty()) {
                    inputEdit_->setText(d);
                    // Auto-compute output path
                    QFileInfo fi(d);
                    QString outPath = fi.absolutePath() + "/arrange_" + fi.fileName();
                    outPathLbl_->setText("Output: " + outPath);
                }
            });
            connect(inputEdit_, &QLineEdit::textChanged, this, [this](const QString& t){
                if (t.trimmed().isEmpty()) { outPathLbl_->setText("Output: (select input folder first)"); return; }
                QFileInfo fi(t.trimmed());
                outPathLbl_->setText("Output: " + fi.absolutePath() + "/arrange_" + fi.fileName());
            });
        }

        // Output path display
        outPathLbl_ = new QLabel("Output: (select input folder first)");
        outPathLbl_->setStyleSheet("QLabel{color:#28f0b0;font-weight:bold;background:rgba(0,60,40,0.4);"
                                  "border:1px solid #0d6040;border-radius:4px;padding:4px 8px;}");
        vbox->addWidget(outPathLbl_);

        // --- Calibration group box (optional) ---
        auto* calibGrp = new QGroupBox("Calibration frames  (optional — leave blank to skip)");
        calibGrp->setStyleSheet(
            "QGroupBox{font-weight:bold;color:#60c8f0;border:1px solid #1a4a6a;"
            "border-radius:6px;margin-top:8px;padding-top:6px;}"
            "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;}");
        auto* cg = new QVBoxLayout(calibGrp);
        cg->setSpacing(6); cg->setContentsMargins(8,10,8,8);

        auto makeCalibRow = [&](const QString& lbl, QLineEdit*& edit, QPushButton*& btn){
            auto* h = new QHBoxLayout;
            auto* l = new QLabel(lbl); l->setFixedWidth(110);
            edit = new QLineEdit; edit->setMinimumHeight(28);
            edit->setPlaceholderText("optional — leave blank to skip");
            btn  = new QPushButton("Browse…"); btn->setFixedWidth(90); btn->setMinimumHeight(28);
            auto* clr = new QPushButton("✕"); clr->setFixedWidth(28); clr->setMinimumHeight(28);
            clr->setToolTip("Clear");
            clr->setStyleSheet("QPushButton{padding:0;font-size:9pt;background:#0c1828;color:#aa4040;"
                               "border:1px solid #2a1a1a;border-radius:3px;}"
                               "QPushButton:hover{background:#1a0808;color:#ff6060;}");
            QLineEdit* ep = edit;
            connect(clr, &QPushButton::clicked, this, [ep](){ ep->clear(); });
            h->addWidget(l); h->addWidget(edit,1); h->addWidget(btn); h->addWidget(clr);
            cg->addLayout(h);
        };

        QPushButton *bBias, *bDark, *bFlat;
        makeCalibRow("Biases folder:", biasEdit_, bBias);
        makeCalibRow("Darks folder:",  darkEdit_, bDark);
        makeCalibRow("Flats folder:",  flatEdit_, bFlat);
        vbox->addWidget(calibGrp);

        auto browseCalib = [this](QLineEdit* e, const QString& title){
            QString d = QFileDialog::getExistingDirectory(this, title, e->text());
            if (!d.isEmpty()) e->setText(d);
        };
        connect(bBias, &QPushButton::clicked, this, [this,browseCalib](){ browseCalib(biasEdit_,"Biases folder"); });
        connect(bDark, &QPushButton::clicked, this, [this,browseCalib](){ browseCalib(darkEdit_,"Darks folder"); });
        connect(bFlat, &QPushButton::clicked, this, [this,browseCalib](){ browseCalib(flatEdit_,"Flats folder"); });

        // --- Options row ---
        auto* optRow = new QHBoxLayout;
        dryChk_ = new QCheckBox("Dry run (simulate, no copy)");
        dryChk_->setToolTip("Shows what would happen without copying any files");
        splitExpChk_ = new QCheckBox("Split by exposure time");
        splitExpChk_->setToolTip("Create subfolders per exposure time: Target/Filter/300s/lights/");
        optRow->addWidget(dryChk_); optRow->addWidget(splitExpChk_);
        optRow->addStretch(1);
        auto* runBtn = new QPushButton("▶  Arrange FITS for Stacking");
        runBtn->setMinimumHeight(38); runBtn->setMinimumWidth(240);
        runBtn->setStyleSheet(
            "QPushButton{font-weight:bold;font-size:10pt;"
            "background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #1468a8,stop:1 #0b5490);"
            "color:#d8f0ff;border:1px solid #1a80cc;}"
            "QPushButton:hover{background:#1a80cc;color:#fff;}");
        optRow->addWidget(runBtn);
        vbox->addLayout(optRow);

        // --- Log ---
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setFont(QFont("Monospace",9));
        log_->setStyleSheet("QPlainTextEdit{background:#060e18;color:#b8d0e8;"
                            "border:1px solid #1a3650;border-radius:5px;}");
        vbox->addWidget(log_,1);

        connect(runBtn, &QPushButton::clicked, this, &ArrangeFitsPanel::runArrange);
    }

private slots:
    void runArrange() {
        QString inputPath = inputEdit_->text().trimmed();
        if (inputPath.isEmpty() || !QDir(inputPath).exists()) {
            QMessageBox::warning(this, "Input", "Select a valid input folder."); return;
        }
        QFileInfo fi(inputPath);
        QString outRoot = fi.absolutePath() + "/arrange_" + fi.fileName();
        bool dryRun     = dryChk_->isChecked();
        bool splitByExp = splitExpChk_->isChecked();

        log_->clear();
        auto logLine = [this](const QString& s){
            log_->appendPlainText(s); QApplication::processEvents();
        };
        logLine(dryRun ? "[DRY RUN] No files will be copied.\n" : "=== Arranging FITS ===");
        logLine("Input:  " + inputPath);
        logLine("Output: " + outRoot + "\n");

        // --- Collect all FITS recursively ---
        QFileInfoList allFits;
        QDirIterator it(inputPath, kFitsExts(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); allFits.append(it.fileInfo()); }
        std::sort(allFits.begin(), allFits.end(), [](const QFileInfo& a, const QFileInfo& b){
            return a.absoluteFilePath() < b.absoluteFilePath();
        });
        logLine(QString("Found %1 FITS file(s).\n").arg(allFits.size()));
        if (allFits.isEmpty()) return;

        // --- Load calibration metadata (if provided) ---
        auto loadCalibDir = [&](const QString& dir) -> QVector<CalibMeta> {
            QVector<CalibMeta> v;
            if (dir.isEmpty() || !QDir(dir).exists()) return v;
            QDirIterator ci(dir, kFitsExts(), QDir::Files, QDirIterator::Subdirectories);
            while (ci.hasNext()) { ci.next(); v.append(readCalibMeta(ci.filePath())); }
            logLine(QString("  Loaded %1 calib frames from: %2")
                    .arg(v.size()).arg(QFileInfo(dir).fileName()));
            return v;
        };
        QVector<CalibMeta> biases = loadCalibDir(biasEdit_->text().trimmed());
        QVector<CalibMeta> darks  = loadCalibDir(darkEdit_->text().trimmed());
        QVector<CalibMeta> flats  = loadCalibDir(flatEdit_->text().trimmed());
        bool hasAnyCalib = !biases.isEmpty() || !darks.isEmpty() || !flats.isEmpty();
        if (hasAnyCalib) logLine("");

        // --- Sort FITS into target/filter[/exp]/lights/ groups ---
        struct GroupInfo { QString safeObj, safeFlt, expSub; QFileInfoList files; };
        QMap<QString, GroupInfo> groups;

        int nCopied=0, nErrors=0;
        for (const QFileInfo& fif : allFits) {
            auto meta    = readFitsObjFilter(fif.absoluteFilePath());
            QString safeObj = safeName(meta.obj);
            QString safeFlt = safeName(meta.filter);
            QString expSub  = splitByExp ? "/" + expFolder(meta.exptime) : "";
            QString key     = safeObj + "/" + safeFlt + expSub;

            QString lightsDir = outRoot + "/" + safeObj + "/" + safeFlt + expSub + "/lights";
            QString dst = lightsDir + "/" + fif.fileName();

            // Handle duplicates
            if (QFile::exists(dst)) {
                QString stem = fif.completeBaseName(), ext = fif.suffix();
                dst = lightsDir + "/" + stem + "_" +
                      QString::number(QDateTime::currentMSecsSinceEpoch()) + "." + ext;
            }

            logLine(QString("  %1 -> %2/%3%4/lights/").arg(fif.fileName(), safeObj, safeFlt, expSub));

            if (!dryRun) {
                QDir().mkpath(lightsDir);
                if (!QFile::copy(fif.absoluteFilePath(), dst)) {
                    logLine("    ERROR: copy failed for " + fif.fileName());
                    nErrors++;
                    continue;
                }
            }
            nCopied++;
            groups[key].safeObj = safeObj;
            groups[key].safeFlt = safeFlt;
            groups[key].expSub  = expSub;
            groups[key].files.append(fif);
        }

        logLine(QString("\nCopied %1 light frame(s) into lights/ folders. Errors: %2\n")
                .arg(nCopied).arg(nErrors));

        // --- Match and copy calibration frames per group ---
        if (!dryRun && hasAnyCalib) {
            logLine("=== Matching calibration frames ===");
            for (auto it2 = groups.begin(); it2 != groups.end(); ++it2) {
                const GroupInfo& g = it2.value();
                QString groupDir = outRoot + "/" + g.safeObj + "/" + g.safeFlt + g.expSub;
                logLine(QString("\n  Group: %1/%2%3").arg(g.safeObj, g.safeFlt, g.expSub));

                // Representative frame (use first light for metadata)
                CalibMeta rep = readCalibMeta(g.files.first().absoluteFilePath());

                // Biases: match gain + sensor size
                if (!biases.isEmpty()) {
                    QVector<CalibMeta> m;
                    for (const auto& b : biases)
                        if (gainMatch(b.gain, rep.gain) &&
                            dimMatch(b.naxis1, b.naxis2, rep.naxis1, rep.naxis2))
                            m.append(b);
                    if (!m.isEmpty()) {
                        QString bDir = groupDir + "/biases";
                        QDir().mkpath(bDir);
                        for (const auto& b : m)
                            QFile::copy(b.path, bDir + "/" + QFileInfo(b.path).fileName());
                        logLine(QString("    Biases: matched %1/%2").arg(m.size()).arg(biases.size()));
                    } else {
                        logLine(QString("    Biases: no gain match (gain=%1)").arg(rep.gain,0,'f',1));
                    }
                }

                // Darks: match gain + sensor size + exposure (relax exposure if no match) — single pass
                if (!darks.isEmpty()) {
                    QVector<CalibMeta> m, mRelaxed;
                    for (const auto& d : darks) {
                        if (gainMatch(d.gain, rep.gain) &&
                            dimMatch(d.naxis1, d.naxis2, rep.naxis1, rep.naxis2)) {
                            if (expMatch(d.exptime, rep.exptime)) m.append(d);
                            else mRelaxed.append(d);
                        }
                    }
                    bool relaxed = m.isEmpty();
                    if (relaxed) m = std::move(mRelaxed);
                    if (!m.isEmpty()) {
                        QString dDir = groupDir + "/darks";
                        QDir().mkpath(dDir);
                        for (const auto& d : m)
                            QFile::copy(d.path, dDir + "/" + QFileInfo(d.path).fileName());
                        if (relaxed)
                            logLine(QString("    Darks: %1 frames (exposure-relaxed match, lights=%2s)")
                                    .arg(m.size()).arg(rep.exptime,0,'f',1));
                        else
                            logLine(QString("    Darks: matched %1/%2 (exp=%3s, gain=%4)")
                                    .arg(m.size()).arg(darks.size())
                                    .arg(rep.exptime,0,'f',1).arg(rep.gain,0,'f',1));
                    } else {
                        logLine("    Darks: no match found");
                    }
                }

                // Flats: match filter + gain (fall back to gain-only) — single pass
                if (!flats.isEmpty()) {
                    QVector<CalibMeta> m, mRelaxed;
                    for (const auto& f : flats) {
                        if (gainMatch(f.gain, rep.gain) &&
                            dimMatch(f.naxis1, f.naxis2, rep.naxis1, rep.naxis2)) {
                            bool fltOk = f.filter.isEmpty() || g.safeFlt.isEmpty() ||
                                         f.filter.compare(g.safeFlt, Qt::CaseInsensitive) == 0;
                            if (fltOk) m.append(f);
                            else mRelaxed.append(f);
                        }
                    }
                    if (m.isEmpty()) {
                        m = std::move(mRelaxed);
                        if (!m.isEmpty()) logLine("    Flats: filter mismatch, using gain-matched flats");
                    }
                    if (!m.isEmpty()) {
                        QString fDir = groupDir + "/flats";
                        QDir().mkpath(fDir);
                        for (const auto& f : m)
                            QFile::copy(f.path, fDir + "/" + QFileInfo(f.path).fileName());
                        logLine(QString("    Flats: matched %1/%2").arg(m.size()).arg(flats.size()));
                    } else {
                        logLine("    Flats: no match found");
                    }
                }
            }
        }

        logLine(dryRun
            ? "\n[DRY RUN complete — no files were copied]"
            : "\n=== Done. Folder is ready for stacking in Siril Stack tab. ===");
        if (!dryRun)
            logLine("Output: " + outRoot);
    }

private:
    QLineEdit      *inputEdit_=nullptr;
    QLineEdit      *biasEdit_=nullptr, *darkEdit_=nullptr, *flatEdit_=nullptr;
    QCheckBox      *dryChk_=nullptr, *splitExpChk_=nullptr;
    QLabel         *outPathLbl_=nullptr;
    QPlainTextEdit *log_=nullptr;
};

// ============================================================
// ToolsPanel — wraps Sort + Stack + Arrange in a sub-tab widget
// ============================================================
class ToolsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolsPanel(QWidget* parent=nullptr) : QWidget(parent) {
        auto* vbox = new QVBoxLayout(this);
        vbox->setContentsMargins(0,0,0,0); vbox->setSpacing(0);
        auto* tabs = new QTabWidget;
        tabs->setDocumentMode(true);
        tabs->addTab(new SortFitsPanel,    "Sort FITS by Object / Filter");
        tabs->addTab(new ArrangeFitsPanel, "Arrange for Stacking");
        tabs->addTab(new SirilStackPanel,  "Stack with Siril");
        vbox->addWidget(tabs);
    }
};

// ============================================================
// MainWindow
// ============================================================
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  MainWindow() {
    cfg_ = loadConfig();

    setStyleSheet(R"(
      QMainWindow { background-color: #080e18; }
      QWidget { background-color: #0c1524; color: #c8ddf0;
        font-family: 'Inter','Ubuntu','Segoe UI',sans-serif; font-size: 10pt; }
      QTabWidget::pane { border: 1px solid #1a3650; border-top: 2px solid #0a7acc; background: #0c1524; }
      QTabBar::tab { background: #091220; color: #5a8aaa; padding: 9px 22px; margin-right: 3px;
        border-top-left-radius: 6px; border-top-right-radius: 6px;
        border: 1px solid #1a3650; border-bottom: none; font-size: 9.5pt; }
      QTabBar::tab:selected { background: #0c1524; color: #00ccff; border-color: #0a7acc;
        border-bottom: none; font-weight: bold; }
      QTabBar::tab:hover:!selected { background: #0f1e34; color: #90b8d4; }
      QListWidget { background-color: #070d18; border: 1px solid #1a3650; border-radius: 6px;
        padding: 4px; font-family: 'Inter',sans-serif;
        font-size: 9.5pt; outline: 0; }
      QListWidget::item { padding: 8px 12px; color: #b8d0e8; background-color: #0c1828;
        border: 1px solid #162840; border-radius: 4px; margin: 1px 2px; }
      QListWidget::item:selected { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #004e9e,stop:1 #006dbf);
        color: #fff; font-weight: bold; border: 1px solid #00aaff; }
      QListWidget::item:hover:!selected { background-color: #122e4a; color: #d8eeff; border: 1px solid #1a5080; }
      QPushButton { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #1670b8,stop:1 #0e5a90);
        color: #eaf6ff; border: 1px solid #1a80cc; padding: 8px 20px; border-radius: 6px;
        font-weight: bold; font-size: 9pt; }
      QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #1a80cc,stop:1 #1670b8);
        border-color: #40aaff; color: #fff; }
      QPushButton:pressed { background: #093c6e; }
      QPushButton:disabled { background: #0c1a28; color: #2a5070; border-color: #1a2a3a; }
      QTextEdit { background-color: #06101c; color: #c0d8ec; border: 1px solid #1a3650;
        border-radius: 6px; padding: 12px;
        font-family: 'JetBrains Mono','Consolas','Courier New',monospace; font-size: 9.5pt; }
      QLabel { background: transparent; color: #7a9abb; padding: 2px 6px; font-size: 9pt; }
      QLineEdit { background-color: #0b1828; color: #60c8f0; border: 1px solid #1a4a6a;
        border-radius: 4px; padding: 4px 8px; }
      QLineEdit:focus { border: 1px solid #00aeff; background-color: #0e2038; }
      QComboBox { background-color: #0b1828; color: #60c8f0; border: 1px solid #1a4a6a;
        border-radius: 4px; padding: 4px 8px; }
      QComboBox::drop-down { border: none; }
      QComboBox QAbstractItemView { background: #0c1828; color: #b8d0e8; border: 1px solid #1a3650; }
      QCheckBox { color: #90b8d4; spacing: 6px; }
      QCheckBox::indicator { width:14px; height:14px; border:1px solid #1a4a6a; border-radius:3px;
        background:#0b1828; }
      QCheckBox::indicator:checked { background:#0a7acc; border-color:#00aeff; }
      QSplitter::handle { background: #1a3650; }
      QScrollBar:vertical { background:#07101c; width:8px; border:none; border-radius:4px; }
      QScrollBar::handle:vertical { background:#1e4a6a; border-radius:4px; min-height:24px; }
      QScrollBar::handle:vertical:hover { background:#2a6090; }
      QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
      QToolTip { background-color:#0e2035; color:#b8d8f0; border:1px solid #1a5070;
        padding:5px 8px; border-radius:4px; }
    )");

    QWidget* central = new QWidget;
    setCentralWidget(central);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);

    // Imaging workflow tabs first (indices 0, 1)
    fitsReviewer_ = new FitsReviewerPanel;
    tabs_->addTab(fitsReviewer_, "FITS Reviewer");
    toolsPanel_ = new ToolsPanel;
    tabs_->addTab(toolsPanel_, "Tools");

    // Target planning tabs (indices 2-6)
    setupTab("Nebula (Ha/SHO)", Category::Nebulae,  nebulaList_,  nebulaMore_);
    setupTab("Galaxies",         Category::Galaxies,  galaxyList_,  galaxyMore_);
    setupTab("Star clusters",    Category::Clusters,  clusterList_, clusterMore_);
    setupTab("Messier",          Category::Messier,   messierList_, messierMore_);
    setupSearchTab();

    connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    info_  = new QTextEdit;  info_->setReadOnly(true);
    plot_  = new SkyPlotWidget;
    plot_->setStyleSheet("QWidget{background-color:#06101c;border:1px solid #1a3650;border-radius:6px;}");

    locationLbl_ = new QLabel("Location: detecting…");
    locationLbl_->setStyleSheet("QLabel{font-weight:bold;color:#28f0b0;background:rgba(0,70,45,0.55);border:1px solid #0d6640;border-radius:5px;padding:4px 10px;}");
    nightLbl_ = new QLabel("Night: …");
    nightLbl_->setStyleSheet("QLabel{font-weight:bold;color:#ffd060;background:rgba(70,52,0,0.55);border:1px solid #7a5c00;border-radius:5px;padding:4px 10px;}");
    dbLbl_ = new QLabel("DB: …");
    dbLbl_->setStyleSheet("QLabel{font-weight:bold;color:#ff9860;background:rgba(70,28,0,0.55);border:1px solid #7a3800;border-radius:5px;padding:4px 10px;}");

    QPushButton* refreshBtn = new QPushButton("Refresh Targets");
    refreshBtn->setMinimumHeight(32);
    connect(refreshBtn,&QPushButton::clicked,this,&MainWindow::refreshNebulaFirst);

    auto* gearLbl=new QLabel("Gear:");
    gearLbl->setStyleSheet("QLabel{color:#60c8f0;font-weight:bold;padding:2px 4px;}");
    profileCombo_=new QComboBox; profileCombo_->setMinimumWidth(260);
    profileCombo_->setStyleSheet("QComboBox{background:#0b1828;color:#c8e0f8;border:1px solid #1a4060;border-radius:4px;padding:3px 8px;min-height:26px;}"
                                  "QComboBox::drop-down{border:none;width:20px;}"
                                  "QComboBox QAbstractItemView{background:#0c1828;color:#b8d0e8;border:1px solid #1a3650;}");
    for(const auto& p:cfg_.gearProfiles) profileCombo_->addItem(p.name);
    profileCombo_->setCurrentIndex(cfg_.activeProfile);
    auto* gearBtn=new QPushButton("⚙ Profiles"); gearBtn->setMinimumHeight(28);
    fovInfoLbl_=new QLabel("FOV: —");
    fovInfoLbl_->setStyleSheet("QLabel{color:#5a90b0;font-weight:bold;padding:3px 10px;min-width:130px;border-radius:4px;}");

    auto updateFovLabel=[this](){
      auto fov=computeFov(cfg_);
      if(fov){
        fovInfoLbl_->setText(QString("FOV: %1'×%2'").arg(fov->first,0,'f',0).arg(fov->second,0,'f',0));
        fovInfoLbl_->setStyleSheet("QLabel{color:#20e8c0;font-weight:bold;padding:3px 10px;min-width:130px;background:rgba(0,80,60,0.5);border:1px solid #0d7050;border-radius:4px;}");
      }else{
        fovInfoLbl_->setText("FOV: —");
        fovInfoLbl_->setStyleSheet("QLabel{color:#5a90b0;font-weight:bold;padding:3px 10px;min-width:130px;border-radius:4px;}");
      }
    };
    connect(profileCombo_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,updateFovLabel](int idx){
      if(idx<0||idx>=cfg_.gearProfiles.size())return;
      cfg_.activeProfile=idx; cfg_.syncFromActiveProfile();
      saveConfig(cfg_); updateFovLabel();
    });
    connect(gearBtn,&QPushButton::clicked,this,[this,updateFovLabel](){
      GearProfileDialog dlg(cfg_,this);
      if(dlg.exec()!=QDialog::Accepted)return;
      cfg_.gearProfiles=dlg.profiles();
      int sel=dlg.selectedIndex();
      cfg_.activeProfile=std::clamp(sel,0,(int)cfg_.gearProfiles.size()-1);
      cfg_.syncFromActiveProfile();
      saveConfig(cfg_);
      profileCombo_->blockSignals(true);
      profileCombo_->clear();
      for(const auto& p:cfg_.gearProfiles)profileCombo_->addItem(p.name);
      profileCombo_->setCurrentIndex(cfg_.activeProfile);
      profileCombo_->blockSignals(false);
      updateFovLabel();
    });
    updateFovLabel();

    QHBoxLayout* top = new QHBoxLayout;
    top->setSpacing(8); top->setContentsMargins(8,8,8,8);
    top->addWidget(locationLbl_,1); top->addWidget(nightLbl_,1); top->addWidget(dbLbl_,1);
    top->addSpacing(8); top->addWidget(gearLbl); top->addWidget(profileCombo_,1);
    top->addWidget(fovInfoLbl_); top->addWidget(gearBtn);
    top->addSpacing(8); top->addWidget(refreshBtn);

    framing_ = new FramingWidget;
    rightStack_   = new QStackedWidget;
    rightStack_->addWidget(plot_); rightStack_->addWidget(info_); rightStack_->addWidget(framing_);
    skyPageBtn_  = new QPushButton("Sky Chart");  skyPageBtn_->setMinimumHeight(34);
    infoPageBtn_ = new QPushButton("Details");    infoPageBtn_->setMinimumHeight(34);
    framingBtn_  = new QPushButton("Framing");    framingBtn_->setMinimumHeight(34);
    skyPageBtn_->setFocusPolicy(Qt::NoFocus); infoPageBtn_->setFocusPolicy(Qt::NoFocus); framingBtn_->setFocusPolicy(Qt::NoFocus);
    QHBoxLayout* navRow = new QHBoxLayout; navRow->setSpacing(4); navRow->setContentsMargins(0,0,0,0);
    navRow->addStretch(1); navRow->addWidget(skyPageBtn_); navRow->addWidget(infoPageBtn_); navRow->addWidget(framingBtn_); navRow->addStretch(1);
    rightPanel_ = new QWidget;
    QVBoxLayout* right = new QVBoxLayout(rightPanel_); right->setSpacing(6); right->setContentsMargins(0,0,0,0);
    right->addWidget(rightStack_,1); right->addLayout(navRow);
    connect(skyPageBtn_,&QPushButton::clicked,this,[this](){switchRightPage(0);});
    connect(infoPageBtn_,&QPushButton::clicked,this,[this](){switchRightPage(1);});
    connect(framingBtn_,&QPushButton::clicked,this,[this](){switchRightPage(2);});
    switchRightPage(0);

    QHBoxLayout* main = new QHBoxLayout; main->setSpacing(12);
    main->addWidget(tabs_,0); main->addWidget(rightPanel_,1);
    QVBoxLayout* root = new QVBoxLayout(central); root->setSpacing(8); root->setContentsMargins(12,12,12,12);
    root->addLayout(top); root->addLayout(main,1);

    setWindowTitle("Astro Toolkit — Target Picker + FITS Reviewer + Tools");
    resize(1440,860);
    if(QScreen* s=QGuiApplication::primaryScreen()){
      QRect g=s->geometry(); move((g.width()-width())/2,(g.height()-height())/2);
    }
    refreshNebulaFirst();
  }

private:
  void setupTab(const QString& title,Category cat,QListWidget*& lOut,QPushButton*& mOut){
    QWidget* page=new QWidget; QVBoxLayout* v=new QVBoxLayout(page); v->setSpacing(8); v->setContentsMargins(8,8,8,8);
    QListWidget* list=new QListWidget; list->setMinimumWidth(420);
    list->setSelectionMode(QAbstractItemView::SingleSelection); list->setAlternatingRowColors(true); list->setSpacing(2);
    QPushButton* more=new QPushButton("Load More"); more->setMinimumHeight(32);
    v->addWidget(list,1); v->addWidget(more);
    tabs_->addTab(page,title); lOut=list; mOut=more;
    connect(list,&QListWidget::itemSelectionChanged,this,[this,cat,list](){onSelectFromList(cat,list);});
    connect(more,&QPushButton::clicked,this,[this,cat,list](){loadMoreList(cat,list);});
  }

  void setupSearchTab(){
    QWidget* page=new QWidget; QVBoxLayout* v=new QVBoxLayout(page); v->setSpacing(8); v->setContentsMargins(8,8,8,8);
    QHBoxLayout* bar=new QHBoxLayout; bar->setSpacing(6);
    searchInput_=new QLineEdit; searchInput_->setPlaceholderText("Object name or catalog ID  (e.g. M42, NGC7000, IC1805)");
    searchInput_->setMinimumHeight(32);
    QPushButton* sb=new QPushButton("Search"); sb->setMinimumHeight(32); sb->setFixedWidth(100);
    bar->addWidget(searchInput_,1); bar->addWidget(sb);
    searchStatusLbl_=new QLabel("Type a name and press Search.");
    searchStatusLbl_->setStyleSheet("QLabel{color:#5a90b0;padding:2px 4px;font-style:italic;}");
    searchList_=new QListWidget; searchList_->setMinimumWidth(420);
    searchList_->setSelectionMode(QAbstractItemView::SingleSelection); searchList_->setAlternatingRowColors(true); searchList_->setSpacing(2);
    searchMore_=new QPushButton("Load More"); searchMore_->setMinimumHeight(32); searchMore_->setEnabled(false);
    v->addLayout(bar); v->addWidget(searchStatusLbl_); v->addWidget(searchList_,1); v->addWidget(searchMore_);
    tabs_->addTab(page,"Search");
    connect(sb,&QPushButton::clicked,this,&MainWindow::runSearch);
    connect(searchInput_,&QLineEdit::returnPressed,this,&MainWindow::runSearch);
    connect(searchList_,&QListWidget::itemSelectionChanged,this,&MainWindow::onSearchItemSelected);
    connect(searchMore_,&QPushButton::clicked,this,&MainWindow::loadMoreSearchResults);
  }

  void switchRightPage(int idx){
    rightStack_->setCurrentIndex(idx);
    const QString on="QPushButton{background:#0c2e50;border:1px solid #0a7acc;color:#00ccff;border-radius:5px;padding:6px 22px;font-weight:bold;}";
    const QString off="QPushButton{background:#080e18;border:1px solid #1a3a5a;color:#4a7090;border-radius:5px;padding:6px 22px;}";
    skyPageBtn_->setStyleSheet(idx==0?on:off);
    infoPageBtn_->setStyleSheet(idx==1?on:off);
    framingBtn_->setStyleSheet(idx==2?on:off);
  }

private slots:
  void refreshNebulaFirst(){
    info_->setText("Refresh started…\n"); setEnabled(false);
    dbLbl_->setText("DB: "+cfg_.sqlitePath);
    if(!QFile::exists(cfg_.sqlitePath)){
      setEnabled(true);
      QMessageBox::critical(this,"SQLite DB missing",
        "DB not found:\n"+cfg_.sqlitePath+"\n\nRun the importer first:\npython import_opengnc_sqlite.py --db \""+appDirPath()+"/catalog.sqlite\" --ngc NGC.csv --addendum addendum.csv --rebuild-aliases");
      return;
    }
    auto fut=QtConcurrent::run([this](){
      auto loc=getBestLocation(cfg_);
      if(!loc.ok){
        QMetaObject::invokeMethod(this,[this](){setEnabled(true);QMessageBox::warning(this,"Location","Could not detect location via gpsd or IP.\nCheck config.json");},Qt::QueuedConnection);
        return;
      }
      auto winOpt=computeTonightWindow(loc.lat,loc.lon);
      if(!winOpt){
        QMetaObject::invokeMethod(this,[this,loc](){
          setEnabled(true);
          locationLbl_->setText(QString("Location: %1,%2 (%3)").arg(loc.lat,0,'f',4).arg(loc.lon,0,'f',4).arg(loc.source));
          QMessageBox::warning(this,"Night","No dark window found in next 24h.");
        },Qt::QueuedConnection);
        return;
      }
      NightWindow win=*winOpt;
      auto neb=fetchCandidatesFromDb(cfg_,Category::Nebulae);
      std::vector<DSO> nebOut; nebOut.reserve(std::min<size_t>(neb.size(),500));
      const double minAlt=cfg_.minAltDeg,minH=cfg_.minHoursAbove;
      const auto fovOpt=fovLongAxisArcmin(cfg_);
      for(auto&o:neb){
        if(maxAltAtTransitDeg(loc.lat,o.decDeg)+1e-9<minAlt)continue;
        if(maxHoursAboveAlt(loc.lat,o.decDeg,minAlt)+1e-9<minH)continue;
        computeTonightForObject(o,win,loc.lat,loc.lon,minAlt);
        if(o.bestRunHoursAbove+1e-9<minH)continue;
        if(fovOpt&&o.majArcmin){double r=*o.majArcmin/ *fovOpt;if(r<0.20||r>0.90)continue;}
        nebOut.push_back(std::move(o));
      }
      std::sort(nebOut.begin(),nebOut.end(),[this](const DSO&a,const DSO&b){return scoreObject(a,Category::Nebulae)>scoreObject(b,Category::Nebulae);});
      QMetaObject::invokeMethod(this,[this,loc,win,nebOut=std::move(nebOut)]()mutable{
        location_=loc; night_=win;
        locationLbl_->setText(QString("Location: %1°,%2° (%3)").arg(loc.lat,0,'f',4).arg(loc.lon,0,'f',4).arg(loc.source));
        nightLbl_->setText(QString("Night: %1 → %2").arg(isoLocal(night_.startUtc)).arg(isoLocal(night_.endUtc)));
        nebulae_=std::move(nebOut); nebShown_=0; nebulaList_->clear(); loadMoreList(Category::Nebulae,nebulaList_);
        galaxies_.clear();clusters_.clear();messier_.clear();
        galLoaded_=cluLoaded_=mesLoaded_=false; galShown_=cluShown_=mesShown_=0;
        galaxyList_->clear();clusterList_->clear();messierList_->clear();
        info_->append("\n=== STATUS ===");
        info_->append("DB: "+cfg_.sqlitePath);
        info_->append(QString("Nebulae tonight: %1").arg((int)nebulae_.size()));
        info_->append("Tip: Click other tabs to load more categories");
        tabs_->setCurrentIndex(2); setEnabled(true); // Nebula tab is now at index 2
      },Qt::QueuedConnection);
    });
    (void)fut;
  }

  void onTabChanged(int idx){
    // Target planning tabs: Galaxies=3, Clusters=4, Messier=5
    if(idx==3&&!galLoaded_)loadCategoryAsync(Category::Galaxies);
    if(idx==4&&!cluLoaded_)loadCategoryAsync(Category::Clusters);
    if(idx==5&&!mesLoaded_)loadCategoryAsync(Category::Messier);
    // FITS Reviewer (0) and Tools (1) use full width — hide the sky chart panel
    bool fullWidth = (idx==0||idx==1);
    rightPanel_->setVisible(!fullWidth);
    tabs_->setSizePolicy(
        fullWidth ? QSizePolicy::Expanding : QSizePolicy::Preferred,
        QSizePolicy::Expanding);
  }

  void loadCategoryAsync(Category cat){
    info_->append("\nLoading category…");
    auto fut=QtConcurrent::run([this,cat](){
      std::vector<DSO> cand=fetchCandidatesFromDb(cfg_,cat);
      std::vector<DSO> out; out.reserve(std::min<size_t>(cand.size(),400));
      const double minAlt=cfg_.minAltDeg,minH=cfg_.minHoursAbove;
      const auto fovOpt=fovLongAxisArcmin(cfg_);
      for(auto&o:cand){
        if(maxAltAtTransitDeg(location_.lat,o.decDeg)+1e-9<minAlt)continue;
        if(maxHoursAboveAlt(location_.lat,o.decDeg,minAlt)+1e-9<minH)continue;
        computeTonightForObject(o,night_,location_.lat,location_.lon,minAlt);
        if(o.bestRunHoursAbove+1e-9<minH)continue;
        if(fovOpt&&o.majArcmin){double r=*o.majArcmin/ *fovOpt;if(r<0.20||r>0.90)continue;}
        out.push_back(std::move(o));
      }
      std::sort(out.begin(),out.end(),[this,cat](const DSO&a,const DSO&b){return scoreObject(a,cat)>scoreObject(b,cat);});
      QMetaObject::invokeMethod(this,[this,cat,out=std::move(out)]()mutable{
        if(cat==Category::Galaxies){galaxies_=std::move(out);galLoaded_=true;galShown_=0;galaxyList_->clear();loadMoreList(Category::Galaxies,galaxyList_);info_->append(QString("Galaxies tonight: %1").arg((int)galaxies_.size()));}
        else if(cat==Category::Clusters){clusters_=std::move(out);cluLoaded_=true;cluShown_=0;clusterList_->clear();loadMoreList(Category::Clusters,clusterList_);info_->append(QString("Clusters tonight: %1").arg((int)clusters_.size()));}
        else if(cat==Category::Messier){messier_=std::move(out);mesLoaded_=true;mesShown_=0;messierList_->clear();loadMoreList(Category::Messier,messierList_);info_->append(QString("Messier tonight: %1").arg((int)messier_.size()));}
      },Qt::QueuedConnection);
    });
    (void)fut;
  }

  std::vector<DSO>& vecFor(Category c){if(c==Category::Nebulae)return nebulae_;if(c==Category::Galaxies)return galaxies_;if(c==Category::Clusters)return clusters_;return messier_;}
  int& shownFor(Category c){if(c==Category::Nebulae)return nebShown_;if(c==Category::Galaxies)return galShown_;if(c==Category::Clusters)return cluShown_;return mesShown_;}

  void loadMoreList(Category cat,QListWidget* list){
    auto&v=vecFor(cat); auto&shown=shownFor(cat);
    int start=shown, end=std::min<int>(shown+cfg_.pageSize,(int)v.size());
    for(int i=start;i<end;i++){
      const auto&o=v[i];
      QString fovTag; auto fovOpt=fovLongAxisArcmin(cfg_);
      if(fovOpt&&o.majArcmin){int pct=(int)std::round(*o.majArcmin/ *fovOpt*100);fovTag=QString("  %1%FOV").arg(pct,3);}
      QString line=QString("%1 [%2]  Alt:%3°  ≥%4h%5").arg(o.display(),-20).arg(o.type,-6).arg((int)o.maxAltDeg,3).arg(o.bestRunHoursAbove,0,'f',1).arg(fovTag);
      QListWidgetItem* item=new QListWidgetItem(line); item->setForeground(QColor(240,240,250)); list->addItem(item);
    }
    shown=end;
  }

  void showTargetDetails(const DSO&o){
    switchRightPage(0);
    plot_->setPath(o.altDegPath,o.azDegPath,o.display()+" (path tonight)");
    // Update framing widget
    if(cfg_.focalLengthMm&&cfg_.pixelSizeUm){
      double sc=206265.0*(*cfg_.pixelSizeUm/1000.0)/ *cfg_.focalLengthMm/60.0;
      double fw=sc*cfg_.sensorWidthPx, fh=sc*cfg_.sensorHeightPx;
      framing_->setTarget(o.display(),o.raDeg,o.decDeg,fw,fh);
    }else{
      framing_->setTarget(o.display(),o.raDeg,o.decDeg,0,0);
    }
    QString fovLine;
    {auto fovOpt=computeFov(cfg_);if(fovOpt&&o.majArcmin){
      double la=std::max(fovOpt->first,fovOpt->second),pct=*o.majArcmin/la*100;
      QString fit=pct>=20&&pct<=90?"good fit":pct<20?"too small":"too large";
      fovLine=QString("  FOV fill: %1% of %2'×%3'  [%4]\n").arg(pct,0,'f',1).arg(fovOpt->first,0,'f',0).arg(fovOpt->second,0,'f',0).arg(fit);
      plot_->setFovLabel(QString("%1% of FOV").arg(pct,0,'f',1));
    } else {plot_->setFovLabel("");}}
    QString basic=QString("=== TARGET: %1 ===\n\nType: %2\nConstellation: %3\n\nVISIBILITY TONIGHT:\n  Max altitude: %4° at %5\n  Window ≥%6°: %7 hours\n\nSIZE: %8' × %9'  |  Vmag: %10\n%11\n")
      .arg(o.display()).arg(o.type).arg(o.constellation.isEmpty()?"Unknown":o.constellation)
      .arg(o.maxAltDeg,0,'f',1).arg(isoLocal(o.tMaxUtc)).arg(cfg_.minAltDeg,0,'f',0).arg(o.bestRunHoursAbove,0,'f',1)
      .arg(o.majArcmin?QString::number(*o.majArcmin,'f',1):"Unknown")
      .arg(o.minArcmin?QString::number(*o.minArcmin,'f',1):"Unknown")
      .arg(o.vmag?QString::number(*o.vmag,'f',1):"Unknown").arg(fovLine);
    info_->setText(basic+"Generating AI imaging plan…\n");
    auto fut=QtConcurrent::run([this,o](){
      if(cfg_.openaiKey.trimmed().isEmpty()){
        QMetaObject::invokeMethod(this,[this](){info_->append("\nOpenAI key missing — check config.json\n");},Qt::QueuedConnection);return;
      }
      QString plan=callOpenAI(cfg_,buildPlannerPrompt(o,location_.lat,location_.lon,cfg_.minAltDeg,cfg_.minHoursAbove,cfg_.activeGear()));
      QMetaObject::invokeMethod(this,[this,plan](){info_->append("\n"+plan+"\n");},Qt::QueuedConnection);
    });
    (void)fut;
  }

  void onSelectFromList(Category cat,QListWidget* list){
    int row=list->currentRow(); if(row<0)return;
    auto&v=vecFor(cat); if(row>=(int)v.size())return;
    showTargetDetails(v[row]);
  }

  void runSearch(){
    const QString term=searchInput_->text().trimmed(); if(term.isEmpty())return;
    searchList_->clear();searchResults_.clear();searchShown_=0;searchMore_->setEnabled(false);
    searchStatusLbl_->setText("Searching…");
    searchStatusLbl_->setStyleSheet("QLabel{color:#ffd060;padding:2px 4px;font-style:italic;}");
    const AppConfig cfg=cfg_; const LocationFix loc=location_; const NightWindow win=night_;
    auto fut=QtConcurrent::run([this,term,cfg,loc,win](){
      auto raw=fetchSearchFromDb(cfg,term);
      for(auto&o:raw)if(loc.ok&&win.startUtc.isValid())computeTonightForObject(o,win,loc.lat,loc.lon,cfg.minAltDeg);
      std::sort(raw.begin(),raw.end(),[](const DSO&a,const DSO&b){
        bool ah=!a.altDegPath.empty(),bh=!b.altDegPath.empty();
        if(ah!=bh)return ah>bh;
        if(fabs(a.maxAltDeg-b.maxAltDeg)>0.01)return a.maxAltDeg>b.maxAltDeg;
        return a.vmag.value_or(99)<b.vmag.value_or(99);
      });
      QMetaObject::invokeMethod(this,[this,raw=std::move(raw),loc]()mutable{
        searchResults_=std::move(raw);searchShown_=0;searchList_->clear();loadMoreSearchResults();
        int n=(int)searchResults_.size();
        if(n==0){searchStatusLbl_->setText("No objects found.");searchStatusLbl_->setStyleSheet("QLabel{color:#ff7060;padding:2px 4px;font-style:italic;}");}
        else{searchStatusLbl_->setText(QString("Found %1 object%2.%3").arg(n).arg(n==1?"":"s").arg(loc.ok?"":" (refresh for sky paths)"));searchStatusLbl_->setStyleSheet("QLabel{color:#28f0b0;padding:2px 4px;font-style:italic;}");}
      },Qt::QueuedConnection);
    });
    (void)fut;
  }

  void loadMoreSearchResults(){
    int start=searchShown_,end=std::min<int>(searchShown_+cfg_.pageSize,(int)searchResults_.size());
    for(int i=start;i<end;i++){
      const auto&o=searchResults_[i];
      QString alt=o.altDegPath.empty()?"  Alt: —   ":QString("  Alt:%1°  ≥%2h").arg((int)o.maxAltDeg,3).arg(o.bestRunHoursAbove,0,'f',1);
      QString fovTag; auto fovOpt=fovLongAxisArcmin(cfg_);
      if(fovOpt&&o.majArcmin){int pct=(int)std::round(*o.majArcmin/ *fovOpt*100);fovTag=QString("  %1%FOV").arg(pct,3);}
      QString line=QString("%1 [%2]%3%4").arg(o.display(),-20).arg(o.type,-6).arg(alt).arg(fovTag);
      QListWidgetItem* item=new QListWidgetItem(line);item->setForeground(QColor(240,240,250));searchList_->addItem(item);
    }
    searchShown_=end; searchMore_->setEnabled(searchShown_<(int)searchResults_.size());
  }

  void onSearchItemSelected(){
    int row=searchList_->currentRow();
    if(row<0||row>=(int)searchResults_.size())return;
    showTargetDetails(searchResults_[row]);
  }

private:
  AppConfig cfg_;
  QLabel *locationLbl_=nullptr,*nightLbl_=nullptr,*dbLbl_=nullptr;
  QComboBox*  profileCombo_=nullptr;
  QLabel *fovInfoLbl_=nullptr;
  QTabWidget* tabs_=nullptr;
  QListWidget *nebulaList_=nullptr,*galaxyList_=nullptr,*clusterList_=nullptr,*messierList_=nullptr;
  QPushButton *nebulaMore_=nullptr,*galaxyMore_=nullptr,*clusterMore_=nullptr,*messierMore_=nullptr;
  QLineEdit*   searchInput_=nullptr;
  QListWidget* searchList_=nullptr;
  QPushButton* searchMore_=nullptr;
  QLabel*      searchStatusLbl_=nullptr;
  std::vector<DSO> searchResults_;
  int searchShown_=0;
  QTextEdit* info_=nullptr;
  SkyPlotWidget* plot_=nullptr;
  FramingWidget* framing_=nullptr;
  QStackedWidget* rightStack_=nullptr;
  QPushButton *skyPageBtn_=nullptr,*infoPageBtn_=nullptr,*framingBtn_=nullptr;
  LocationFix location_;
  NightWindow night_;
  std::vector<DSO> nebulae_,galaxies_,clusters_,messier_;
  int nebShown_=0,galShown_=0,cluShown_=0,mesShown_=0;
  bool galLoaded_=false,cluLoaded_=false,mesLoaded_=false;
  QWidget*           rightPanel_=nullptr;
  FitsReviewerPanel* fitsReviewer_=nullptr;
  ToolsPanel*        toolsPanel_=nullptr;
};

#include "main.moc"

int main(int argc,char** argv){
  QApplication app(argc,argv);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  MainWindow w; w.show();
  int rc=app.exec();
  curl_global_cleanup();
  return rc;
}

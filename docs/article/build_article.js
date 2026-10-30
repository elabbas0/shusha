// Builds shusha_meqale.docx in the layout of AzTU "Elmi əsərlər" (Proceedings):
// A4, 2 cm margins, Times New Roman 12 pt, single spacing, 1 cm indent,
// Azerbaijani title block, English title/abstract, run-in section headings,
// ƏDƏBİYYAT, then the Azerbaijani Xülasə at the end.
const fs = require("fs");
const path = require("path");
const {
  Document, Packer, Paragraph, TextRun, ImageRun, Table, TableRow, TableCell,
  AlignmentType, WidthType, BorderStyle, Header, Footer, PageNumber, TabStopType,
} = require("docx");

const DIR = __dirname;
const FONT = "Times New Roman";
const TEXT_W = 11906 - 2 * 1134; // A4 width minus 2 cm margins, in DXA
const INDENT = 567;               // 1 cm

// Minimal inline markup: **bold**, *italic*, _{subscript}
function runs(text, base = {}) {
  const out = [];
  let bold = false, italic = false, buf = "";
  const flush = () => {
    if (buf) out.push(new TextRun({ text: buf, bold: bold || base.bold, italics: italic || base.italics, size: base.size, font: FONT }));
    buf = "";
  };
  for (let i = 0; i < text.length; i++) {
    if (text.startsWith("**", i)) { flush(); bold = !bold; i++; continue; }
    if (text[i] === "*") { flush(); italic = !italic; continue; }
    if (text.startsWith("_{", i)) {
      flush();
      const end = text.indexOf("}", i);
      out.push(new TextRun({ text: text.slice(i + 2, end), subScript: true, bold: bold || base.bold, italics: italic || base.italics, size: base.size, font: FONT }));
      i = end;
      continue;
    }
    buf += text[i];
  }
  flush();
  return out;
}

const P = (text, opt = {}) => new Paragraph({
  alignment: opt.align ?? AlignmentType.JUSTIFIED,
  indent: opt.indent === undefined ? { firstLine: INDENT } : opt.indent,
  spacing: { line: 240, before: 0, after: 0 },
  keepNext: opt.keepNext,
  children: runs(text, { size: opt.size ?? 24, bold: opt.bold, italics: opt.italics }),
});
const blank = () => new Paragraph({ spacing: { line: 240 }, children: [new TextRun({ text: "", font: FONT, size: 24 })] });
const center = (text, o = {}) => P(text, { ...o, align: AlignmentType.CENTER, indent: {} });
const left = (text, o = {}) => P(text, { ...o, align: AlignmentType.LEFT, indent: {} });

function formula(expr, num) {
  return new Paragraph({
    tabStops: [
      { type: TabStopType.CENTER, position: Math.round(TEXT_W / 2) },
      { type: TabStopType.RIGHT, position: TEXT_W },
    ],
    spacing: { line: 240, before: 120, after: 120 },
    children: [new TextRun({ text: "\t", font: FONT }), ...runs(expr, { size: 24, italics: true }),
      new TextRun({ text: `\t(${num})`, font: FONT, size: 24 })],
  });
}

function figure(file, w, h, caption) {
  return [
    blank(),
    new Paragraph({
      alignment: AlignmentType.CENTER, keepNext: true,
      children: [new ImageRun({ type: "png", data: fs.readFileSync(path.join(DIR, file)), transformation: { width: w, height: h },
        altText: { title: caption, description: caption, name: file } })],
    }),
    P(caption, { align: AlignmentType.CENTER, indent: {}, italics: true }),
    blank(),
  ];
}

const border = { style: BorderStyle.SINGLE, size: 4, color: "000000" };
const borders = { top: border, bottom: border, left: border, right: border };

function table(label, title, widths, rows) {
  const cell = (t, w, head, last) => new TableCell({
    borders, width: { size: w, type: WidthType.DXA },
    margins: { top: 40, bottom: 40, left: 100, right: 100 },
    children: [new Paragraph({ alignment: head ? AlignmentType.CENTER : AlignmentType.LEFT, keepNext: !last,
      children: runs(t, { size: 22, bold: head }) })],
  });
  return [
    blank(),
    P(label, { align: AlignmentType.RIGHT, indent: {}, italics: true, keepNext: true }),
    P(title, { align: AlignmentType.CENTER, indent: {}, bold: true, keepNext: true }),
    new Table({
      width: { size: widths.reduce((a, b) => a + b, 0), type: WidthType.DXA },
      columnWidths: widths,
      rows: rows.map((r, i) => new TableRow({ tableHeader: i === 0, cantSplit: true, children: r.map((t, j) => cell(t, widths[j], i === 0, i === rows.length - 1)) })),
    }),
    blank(),
  ];
}

// ---------------------------------------------------------------------------
const AUTHOR_AZ = "Əlabbas [ata adı] oğlu Abdullayev";
const AUTHOR_EN = "Alabbas [patronymic] Abdullayev";
const SHORT = "Ə.Abdullayev";
const TITLE_AZ = "SHUSHA: RÜTBƏYƏ ƏSASLANAN ŞİFRƏLƏMƏ VƏ İCAZƏ MODELİNƏ MALİK TƏHLÜKƏSİZ ÇOXİSTİFADƏÇİLİ ŞƏBƏKƏ ƏMƏLİYYAT SİSTEMİ";
const TITLE_EN = "SHUSHA: A SECURE MULTI-USER NETWORK OPERATING SYSTEM WITH A RANK-BASED ENCRYPTION AND PERMISSION MODEL";

const ABSTRACT_EN = "**Abstract.** The article presents Shusha, a security-oriented multi-user network operating system built on the ICDA kernel, and its central component, the Workspace: an encrypted shared environment that only invited Shusha devices can join. The first member to create a workspace becomes its administrator, and members who join later receive lower ranks; a lower-ranked member needs explicit permission to view or edit a higher-ranked member's files, while higher ranks do not. Instead of inventing new ciphers, the system composes standardized primitives (XChaCha20-Poly1305, X25519 with ML-KEM-768, Ed25519, HKDF, Argon2id) into its own key hierarchy: a random key per file, rank keys that form a downward chain, and signatures that make edit rights enforceable even against modified clients. Device keys are random and bound to the hardware identifier and the user's password, so no TPM is required. The design also covers real-time co-editing with rank-led sessions, operation over a LAN and the internet without paid servers, and continued operation while the administrator is offline. The prototype implements a kernel CSPRNG, a cryptographic library verified by 143 test-vector checks and a damage-tracking window manager; the analysis also revealed and removed predictable key generation in the inherited TLS stack.";
const KEYWORDS_EN = "***Keywords:*** *operating system security, multi-user systems, key hierarchy, authenticated encryption, access control, collaborative editing.*";

const XULASE = "**Xülasə.** Məqalədə ICDA nüvəsi əsasında qurulmuş təhlükəsiz çoxistifadəçili şəbəkə əməliyyat sistemi Shusha və onun mərkəzi komponenti olan iş məkanı (Workspace) təqdim olunur. İş məkanı yalnız dəvət edilmiş Shusha qurğularının qoşula bildiyi şifrələnmiş ortaq mühitdir. Onu ilk yaradan iştirakçı inzibatçı olur, sonra qoşulanlar isə növbə ilə aşağı rütbələr alır; aşağı rütbəli iştirakçı yuxarı rütbəlinin faylına baxmaq və ya onu redaktə etmək üçün açıq icazə almalıdır, yuxarı rütbələr isə icazəsiz işləyir. Sistem yeni şifr ixtira etmir: standart primitivləri (XChaCha20-Poly1305, X25519 və ML-KEM-768, Ed25519, HKDF, Argon2id) öz açar iyerarxiyasında birləşdirir – hər fayl üçün təsadüfi açar, aşağıya doğru zəncir təşkil edən rütbə açarları və redaktə hüququnu dəyişdirilmiş kliyentlərə qarşı da təmin edən imzalar. Qurğu açarları təsadüfi yaradılır və aparat identifikatoruna, həmçinin istifadəçi parolu ilə bağlanır, buna görə TPM tələb olunmur. Layihə rütbəyə əsaslanan sessiyalarla real vaxtda birgə redaktəni, ödənişli serverlər olmadan yerli şəbəkə və internet üzərindən işləməni və inzibatçı oflayn olduqda işin davamını əhatə edir. Prototipdə nüvənin kriptoqrafik təsadüfi ədədlər generatoru, 143 test-vektor yoxlaması ilə təsdiqlənmiş kriptoqrafik kitabxana və zədələnmiş sahələri izləyən pəncərə meneceri reallaşdırılmış, irsən alınmış TLS kodunda proqnozlaşdırıla bilən açar yaradılması aşkar edilərək aradan qaldırılmışdır.";
const ACAR = "***Açar sözlər:*** *əməliyyat sistemlərinin təhlükəsizliyi, çoxistifadəçili sistemlər, açar iyerarxiyası, autentifikasiyalı şifrələmə, giriş nəzarəti, birgə redaktə.*";

const body = [
  P("**Giriş.** Komanda işinin getdikcə daha çox paylanmış şəkildə – müxtəlif kompüterlərdə, yerli şəbəkədə və internet üzərindən – aparılması məlumatların məxfiliyi və bütövlüyü məsələlərini əməliyyat sistemi səviyyəsinə gətirir. Ənənəvi yanaşmada fayllar ya mərkəzi serverdə saxlanılır və onların qorunması serverin inzibatçısına həvalə olunur, ya da ayrı-ayrı tətbiqlər öz şifrələmə mexanizmlərini qurur. Hər iki halda təhlükəsizlik sistemin ən zəif həlqəsi ilə məhdudlaşır: serverə daxil olan şəxs bütün məlumatları görür, tətbiqlər isə əməliyyat sisteminin etibarı yoxlanılmamış komponentləri üzərində işləyir."),
  P("Son illərdə süni intellekt əsaslı alətlərin proqram təminatındakı zəiflikləri avtomatik aşkar etmək qabiliyyəti əhəmiyyətli dərəcədə artmışdır. Bu, mühafizə sistemlərinin layihələndirilməsinə yeni tələb qoyur: hücumçu artıq yalnız riyazi kriptoanaliz deyil, həm də tətbiqetmə xətalarını – proqnozlaşdırıla bilən təsadüfi ədədləri, yoxlanılmayan imzaları, protokol vəziyyətlərinin qarışdırılmasını – sürətlə axtara bilir. Kerkhoffs prinsipinə görə sistemin təhlükəsizliyi alqoritmin gizliliyinə deyil, yalnız açarın gizliliyinə əsaslanmalıdır [1]. Buradan çıxan praktik nəticə ondan ibarətdir ki, heç kimin, o cümlədən süni intellektin aça bilməyəcəyi şifrələmə yeni şifr ixtira etməklə deyil, geniş tədqiq olunmuş standart primitivləri düzgün tətbiq etməklə və onları əhatə edən proqram kodunu minimal və yoxlanıla bilən saxlamaqla əldə olunur."),
  P("Əməliyyat sistemlərində informasiyanın mühafizəsinin klassik prinsipləri – ən az imtiyaz, tam vasitəçilik, mexanizmin sadəliyi – Saltzer və Şröder tərəfindən formalaşdırılmışdır [2]. Səviyyələrə əsaslanan giriş modelinin ən tanınmış nümunəsi isə Bell–LaPadula modelidir: subyekt öz səviyyəsindən aşağı və ya ona bərabər səviyyəli obyektləri oxuya bilər [3]. Təqdim olunan işdə bu ideya kriptoqrafik formada reallaşdırılır: giriş hüququ inzibati qeyd kimi deyil, açarın mövcudluğu kimi ifadə olunur və buna görə də dəyişdirilmiş kliyent proqramı tərəfindən aşıla bilmir."),

  P("**Məsələnin qoyuluşu.** Tədqiqatın məqsədi ICDA nüvəsi [4] əsasında çoxistifadəçili şəbəkə əməliyyat sistemi – Shusha – yaratmaq və onun mərkəzi komponenti olan iş məkanı (Workspace) üçün şifrələmə və icazə modelini işləyib hazırlamaqdır. Sistem aşağıdakı tələbləri ödəməlidir:"),
  P("1) iş məkanına yalnız dəvət edilmiş Shusha qurğuları qoşula bilər;"),
  P("2) iş məkanını ilk yaradan istifadəçi inzibatçı (rütbə 0) olur, sonra qoşulanlar növbə ilə aşağı rütbələr alır;"),
  P("3) aşağı rütbəli istifadəçi yuxarı rütbəlinin faylına baxmaq və ya onu redaktə etmək üçün icazə almalıdır, yuxarı rütbəli isə icazəsiz baxa və redaktə edə bilər;"),
  P("4) sistem həm yerli şəbəkədə, həm də internet üzərindən, ödənişli serverlər olmadan işləməli və inzibatçının kompüteri söndürüldükdə dayanmamalıdır;"),
  P("5) real vaxtda birgə redaktə dəstəklənməlidir: aşağı rütbəli iştirakçını redaktə sessiyasına yuxarı rütbəli redaktor buraxır, faylı oxuya bilən hər kəs isə hazırda kimin redaktə etdiyini görür;"),
  P("6) TPM modulu tələb olunmamalı, qurğu açarı aparat identifikatoruna bağlanmalıdır."),
  P("Hücumçu modeli olaraq şəbəkə hücumçusu, Shusha-nı imitasiya edən dəyişdirilmiş kliyent, zərərli iştirakçı, iş məkanından çıxarılmış iştirakçı, oğurlanmış disk və etibarsız saxlama xidməti nəzərdən keçirilir. Nüvə səviyyəsində ələ keçirilmiş kompüter isə model xaricindədir, çünki belə halda heç bir şifrələmə həmin istifadəçinin gördüyü məlumatları qoruya bilməz."),

  P("**Həll metodları.** Shusha ICDA layihəsinin yalnız nüvəsini və drayverlərini götürür; istifadəçi sahəsi – init prosesi, komanda interpretatoru, pəncərə meneceri və tətbiqlər – yenidən yazılmışdır (Şəkil 1). ICDA-nın sistem çağırışları interfeysi dondurulduğundan yeni imkanlar nüvəyə yeni çağırış kimi deyil, mövcud mesaj növbələri və paylaşılan yaddaş üzərində işləyən istifadəçi xidmətləri (keyd, authd, netd, workspaced) şəklində əlavə olunur. Nüvədə yalnız onun xaricində etibarlı həyata keçirilə bilməyən funksiyalar qalır: kriptoqrafik təsadüfi ədədlər generatoru, giriş nəzarəti və şəbəkə soketləri. Gizli açarlar yalnız keyd prosesində saxlanılır; digər proseslər ondan imzalamağı və ya açarı açmağı xahiş edir, lakin açıq şəkildə açarları heç vaxt almır."),
  ...figure("fig1_architecture.png", 620, 335, "Şəkil 1. Shusha-nın arxitekturası (bütöv xətt – reallaşdırılmış, qırıq xətt – layihələndirilmiş komponentlər)"),
  P("Sistemdə istifadə olunan bütün primitivlər beynəlxalq standartlara əsaslanır (Cədvəl 1). Saxlanılan məlumatlar üçün XChaCha20-Poly1305 seçilmişdir: eyni fayl açarı ilə çox sayda istifadəçi yazdığından 192 bitlik təsadüfi nonce sayğacların koordinasiyası olmadan təkrarlanma ehtimalını praktik olaraq sıfıra endirir [5, 6]. Açarların törədilməsi HKDF [7], imzalar Ed25519 [8], kvantdan sonrakı açar mübadiləsi ML-KEM-768 [9], parolların heşlənməsi Argon2id [10] ilə aparılır."),
  ...table("Cədvəl 1", "Shusha-da istifadə olunan kriptoqrafik primitivlər", [3600, 3138, 2900], [
    ["Təyinat", "Primitiv", "Standart"],
    ["Saxlanılan məlumatların şifrələnməsi", "XChaCha20-Poly1305", "draft-irtf-cfrg-xchacha [6]"],
    ["Nəqliyyat şifrələməsi", "ChaCha20-Poly1305", "RFC 8439 [5]"],
    ["Açarların törədilməsi", "HKDF-SHA-256", "RFC 5869 [7]"],
    ["Rəqəmsal imza", "Ed25519", "RFC 8032 [8]"],
    ["Açar mübadiləsi", "X25519 + ML-KEM-768", "FIPS 203 [9]"],
    ["Parolların heşlənməsi", "Argon2id", "RFC 9106 [10]"],
    ["Nəqliyyat protokolu", "Noise XX / IK", "Noise Framework [11]"],
  ]),
  P("Açarın müəyyən iştirakçı üçün bükülməsi (wrap) hibrid sxem üzrə aparılır: birdəfəlik X25519 açar cütü *e* yaradılır, alıcının açıq açarları ilə iki ortaq sirr hesablanır və açar şifrələmə açarı (1) düsturu ilə törədilir:"),
  formula("KEK = HKDF(ss_{1} ‖ ss_{2};  e_{pub} ‖ c;  \"shusha-wrap-v1\" ‖ id ‖ məqsəd),", 1),
  P("burada *ss*_{1} = X25519(*e*, *pk*_{X}), (*c*, *ss*_{2}) = ML-KEM-768.Encaps(*pk*_{K}), *id* – alıcı açarının identifikatoru, *məqsəd* isə açarın aid olduğu kontekstdir (məsələn, fayl və onun dövrü). Bükülmüş açarı bərpa etmək üçün hücumçu hər iki problemi – elliptik əyri üzərində diskret loqarifmi və modul qəfəslərdə öyrənmə problemini – həll etməlidir; bu, sistemi “indi topla, sonra aç” tipli kvant hücumlarından qoruyur.", { indent: {} }),
  P("Qurğu açarının özü aparat identifikatorundan törədilmir, əksinə təsadüfi yaradılır, çünki aparat identifikatorları gizli deyil: onlar istənilən proqram tərəfindən oxunur və çox vaxt qurğunun etiketində çap olunur. Aparat yalnız açarın diskdə saxlanılan şifrəli surətini kompüterə bağlayır:"),
  formula("K_{wrap} = HKDF(Argon2id(parol, duz) ‖ K_{hw};  \"shusha-device-v1\"),", 2),
  P("burada *K*_{hw} – Şamir sxemi ilə üç hissəyə bölünmüş və istənilən iki aparat komponenti (SMBIOS UUID, şəbəkə kartının MAC ünvanı, diskin seriya nömrəsi) ilə bərpa olunan sirdir. Başqa kompüterə köçürülmüş disk aparat yoxlamasından, aparat identifikatorunun saxtalaşdırılması isə paroldan keçə bilmir; bir komponentin dəyişdirilməsi isə girişi bağlamır. TPM mövcud olduqda əlavə qat kimi istifadə olunur.", { indent: {} }),
  P("Hər *r* rütbəsi üçün *RK*_{r} rütbə açarı yaradılır. *RK*_{r} həmin rütbədəki hər iştirakçının açıq açarı üçün bükülür və əlavə olaraq *RK*_{r−1} ilə şifrələnir; beləliklə *RK*_{j} açarına malik iştirakçı *j*-dən aşağı bütün rütbələrin açarlarını ardıcıl aça bilir (Şəkil 2). Rütbəsi *r* olan iştirakçının yaratdığı hər fayl üçün təsadüfi *FK* açarı yaradılır və o, sahibin açarı, *RK*_{r−1} (yəni bütün yuxarı rütbələr) və icazə verilmiş iştirakçılar üçün bükülür. Nəticədə yuxarı rütbələr aşağı rütbələrin fayllarını icazəsiz oxuyur, aşağı və bərabər rütbələr isə yalnız açıq verilmiş icazə ilə; bu, Bell–LaPadula modelindəki “yuxarıya oxuma qadağası” prinsipinin kriptoqrafik ifadəsidir [3]."),
  ...figure("fig2_keys.png", 620, 254, "Şəkil 2. Rütbə açarları zənciri və fayl açarının bükülməsi"),
  P("Oxuma hüququ kriptoqrafiya ilə təmin olunur: iştirakçının ya bükülmüş açarı var, ya da yoxdur. Redaktə hüququ isə imzalarla təmin olunur: hər dəyişiklik redaktorun qurğu açarı ilə imzalanır, digər iştirakçılar isə imzanı iş məkanının üzvlük jurnalına əsasən yoxlayır. Üzvlük jurnalı yalnız əlavə edilən və hər qeydi əvvəlkinin heşini saxlayan zəncirdir; dəvətlər, rütbə dəyişiklikləri, icazələr və çıxarılmalar burada imzalanmış qeydlər kimi saxlanılır. Dəyişdirilmiş kliyent öz lokal surətini istədiyi kimi dəyişə bilər, lakin vicdanlı iştirakçıları icazəsiz redaktəni qəbul etməyə məcbur edə bilməz. İştirakçı çıxarıldıqda onun aça bildiyi rütbə və fayl açarları yenilənir; çıxarılmış iştirakçı yalnız əvvəllər gördüyü məlumatları saxlayır, bu isə prinsipcə aradan qaldırıla bilməyən məhdudiyyətdir."),
  P("Sənədlər münaqişəsiz təkrarlanan verilənlər tipi (CRDT) kimi saxlanılır və bu, mərkəzi server olmadan eyni vaxtlı dəyişikliklərin eyni nəticəyə birləşməsini təmin edir [12]. Dəyişikliklər təxminən 150 ms-lik paketlərlə toplanır, sessiya açarı ilə (*SK* = HKDF(*FK*, sessiya identifikatoru)) şifrələnir və şifrəli mətn üzərindən imzalanır, buna görə ötürücü qovşaqlar məzmunu görmədən müəllifin səlahiyyətini yoxlaya bilir. Redaktə sessiyasına iştirakçılar arasında ən yüksək rütbəli şəxs rəhbərlik edir: ondan yüksək rütbəli istifadəçi dərhal qoşulur, bərabər və ya aşağı rütbəli istifadəçi isə rəhbərin təsdiqini gözləyir. Faylı oxuya bilən hər kəs aktiv redaktorların siyahısını görür və bu siyahını inzibatçı da gizlədə bilməz."),
  P("Nəqliyyat səviyyəsində WireGuard-da da istifadə olunan Noise protokol çərçivəsi [11, 13] hibrid ML-KEM addımı ilə tətbiq edilir. Yerli şəbəkədə qurğular bir-birini iş məkanının gizli açarından və cari saatdan törədilmiş HMAC identifikatorlu yayım mesajları ilə tapır; kənar müşahidəçi bu identifikatoru təsadüfi baytlardan ayıra bilmir. İnternet üzərindən ictimai ünvan pulsuz STUN serverləri vasitəsilə müəyyən edilir [14], qovşaqların yeri isə Kademlia əsaslı ictimai BitTorrent DHT şəbəkəsində şifrələnmiş qeyd kimi dərc olunur [15]. Ödənişli bulud xidməti əvəzinə iş məkanının bütün iştirakçıları, o cümlədən oxuma hüququ olmayan saxlama qovşaqları şifrəli məlumatların surətlərini saxlayır: ən azı bir qovşaq onlayn olduqda sinxronizasiya mümkündür, inzibatçının səlahiyyətləri isə əvvəlcədən imzalanmış varislik qeydi ilə müavinə ötürülə bilir."),

  P("**Aprobasiya.** Layihənin birinci mərhələsində aşağıdakı komponentlər reallaşdırılmış və QEMU virtual maşınında sınaqdan keçirilmişdir. Birincisi, nüvəyə kriptoqrafik təsadüfi ədədlər generatoru əlavə edilmişdir: başlanğıc entropiya RDSEED/RDRAND təlimatlarından (prosessor dəstəklədikdə) və vaxt titrəməsindən (jitter) toplanır, SHA-256 ilə qarışdırılır və açarın sürətli silinməsi prinsipi ilə işləyən ChaCha20 əsaslı generatora ötürülür; hər sorğuda generatora təzə hadisə məlumatları əlavə edilir. Generator /dev/random qurğusu vasitəsilə istifadəçi proseslərinə də təqdim olunur."),
  P("İkincisi, irsən alınmış TLS kodunun təhlili zamanı ciddi zəiflik aşkar edilmişdir: X25519 gizli açarı, müştəri təsadüfi ədədi, CBC inisializasiya vektorları və RSA ilkin sirri planlayıcının taymer sayğacından törədilirdi, yəni yükləmə vaxtını təxmin edən hücumçu bu açarları bərpa edə bilərdi. Bütün bu yerlər yeni generatora keçirilmişdir. Bu nəticə yeni şifr ixtira etmək əvəzinə tətbiqetmə səviyyəsində təhlükəsizliyə diqqət yetirmək zərurətini əyani şəkildə təsdiq edir."),
  P("Üçüncüsü, nüvədə, istifadəçi proqramlarında və host sistemində eyni mənbə kodundan yığılan kriptoqrafik kitabxana hazırlanmışdır: ChaCha20, Poly1305, ChaCha20-Poly1305, XChaCha20-Poly1305, SHA-256/512, HMAC, HKDF, Ed25519 və təsadüfi ədədlər generatoru. Ed25519 reallaşdırması TweetNaCl kitabxanasının strukturuna əsaslanır [16], əyri sabitləri isə ilkin prinsiplərdən hesablanmışdır. Kitabxana 143 yoxlamadan ibarət test dəsti ilə sınanmışdır: RFC 8439, RFC 5869 və RFC 8032 standartlarının rəsmi test vektorları, müstəqil reallaşdırmalardan (pyca/cryptography, libsodium) alınmış vektorlar, həmçinin dəyişdirilmiş şifrəli mətnin, teqin, imzanın və qeyri-kanonik *S* qiymətinin rədd edilməsi halları. Bütün yoxlamalar GCC və Clang kompilyatorları ilə, həmçinin AddressSanitizer və UndefinedBehaviorSanitizer alətləri altında uğurla keçmişdir. Ölçülmüş məhsuldarlıq Cədvəl 2-də verilmişdir."),
  ...table("Cədvəl 2", "Kriptoqrafik kitabxananın məhsuldarlığı (Intel Xeon 2,1 GHz, bir nüvə, GCC -O2)", [5638, 4000], [
    ["Əməliyyat", "Nəticə"],
    ["XChaCha20-Poly1305 ilə şifrələmə", "≈ 209 MB/s"],
    ["SHA-256 ilə heşləmə", "≈ 124 MB/s"],
    ["Ed25519 imzalama", "≈ 800–900 əməliyyat/s"],
    ["Ed25519 imzasının yoxlanılması", "≈ 450 əməliyyat/s"],
  ]),
  P("Simmetrik əməliyyatların sürəti diskdən və şəbəkədən gələn məlumatların şifrələnməsi üçün kifayətdir. Ed25519 reallaşdırması sadəlik və yoxlanıla bilmə naminə sürətdən imtina edir; 150 ms-lik redaktə paketləri hər redaktordan saniyədə təxminən 7 imza tələb etdiyindən bu göstərici birgə redaktə üçün də kifayət qədər ehtiyata malikdir."),
  P("Dördüncüsü, yeni pəncərə meneceri yalnız zədələnmiş düzbucaqlıları yenidən kompozisiya edir və kadr buferinə yalnız onları köçürür: məsələn, 800×600 ekranda kursorun hərəkəti tam kadrın 480 000 pikseli əvəzinə iki 12×19 düzbucaqlının (456 piksel) yenilənməsini tələb edir. Tətbiqlər öz buferlərində çəkir və pəncərə menecerinə yalnız dəyişmiş sahəni ötürür. ICDA-dan Explorer fayl meneceri və Terminal tətbiqi bu yeni sistemə köçürülmüşdür. İş zamanı nüvənin paylaşılan yaddaş və mesaj növbələrində giriş nəzarətinin olmadığı da müəyyən edilmişdir: istənilən proses istənilən paylaşılan yaddaş sahəsini öz ünvan sahəsinə qoşa bilir. Bu çatışmazlıq çoxistifadəçili rejimə keçiddən əvvəl prosesə məxsus deskriptor cədvəlləri ilə aradan qaldırılmalıdır."),

  P("**Nəticələrin tətbiqi.** Təklif olunan model məlumatların məxfiliyi vacib olan, lakin mərkəzi infrastruktura vəsait ayırmaq imkanı məhdud olan kollektivlər üçün nəzərdə tutulub: universitet laboratoriyaları və tədqiqat qrupları, kiçik mühəndis və proqramlaşdırma komandaları, layihə sənədləri üzərində birgə işləyən təşkilatlar. Rütbə modeli təşkilati iyerarxiyanı – rəhbər, aparıcı mütəxəssis, icraçı – birbaşa əks etdirir, serversiz işləmə və pulsuz sinxronizasiya isə sistemin internet bağlantısı qeyri-sabit olan şəraitdə də tətbiqini mümkün edir. Kriptoqrafik kitabxana və onun test dəsti digər açıq layihələrdə müstəqil şəkildə istifadə oluna bilər."),

  P("**Nəticə.** Məqalədə ICDA nüvəsi əsasında qurulan Shusha təhlükəsiz çoxistifadəçili şəbəkə əməliyyat sistemi və onun iş məkanı üçün rütbəyə əsaslanan şifrələmə və icazə modeli təqdim edilmişdir. Göstərilmişdir ki, heç kimin aça bilməyəcəyi şifrələməyə yeni şifr ixtirası ilə deyil, standart primitivlərin öz açar iyerarxiyası çərçivəsində birləşdirilməsi və tətbiqetmə xətalarının aradan qaldırılması ilə nail olmaq mümkündür."),
  P("Tədqiqatın elmi yeniliyi oxuma hüququnun açar zənciri, redaktə hüququnun isə imza və üzvlük jurnalı ilə ifadə olunduğu, yuxarı rütbələrin aşağı rütbələrə münasibətdə icazəsiz, aşağı rütbələrin isə yalnız açıq icazə ilə işlədiyi modelin rütbəyə əsaslanan birgə redaktə sessiyaları və TPM-siz aparata bağlanan qurğu açarları ilə birlikdə işlənib hazırlanmasındadır. Prototipdə kriptoqrafik təsadüfi ədədlər generatoru, 143 yoxlama ilə təsdiqlənmiş kriptoqrafik kitabxana və yeni pəncərə meneceri reallaşdırılmış, irsən alınmış TLS kodundakı proqnozlaşdırıla bilən açar yaradılması aradan qaldırılmışdır. Növbəti mərhələlərdə nüvədə istifadəçi identifikatorlarının məcburi tətbiqi, fayl sisteminə sahib və giriş rejimi atributlarının əlavə edilməsi, ML-KEM və Argon2id primitivlərinin, Noise əsaslı nəqliyyatın və iş məkanı protokolunun reallaşdırılması nəzərdə tutulur."),
];

const refs = [
  "Kerckhoffs A. La cryptographie militaire. Journal des sciences militaires, 1883, vol. IX, s.5–38.",
  "Saltzer J.H., Schroeder M.D. The Protection of Information in Computer Systems. Proceedings of the IEEE, 1975, vol. 63, №9, s.1278–1308.",
  "Bell D.E., LaPadula L.J. Secure Computer Systems: Mathematical Foundations. Technical Report MTR-2547, vol. I. Bedford, MITRE Corporation, 1973.",
  "Abdullayev Ə. ICDA operating system: source code repository. GitHub, 2026. https://github.com/elabbas0/ICDA",
  "Nir Y., Langley A. ChaCha20 and Poly1305 for IETF Protocols. RFC 8439, IETF, 2018.",
  "Arciszewski S. XChaCha: eXtended-nonce ChaCha and AEAD_XChaCha20_Poly1305. Internet-Draft draft-irtf-cfrg-xchacha-03, IETF, 2020.",
  "Krawczyk H., Eronen P. HMAC-based Extract-and-Expand Key Derivation Function (HKDF). RFC 5869, IETF, 2010.",
  "Josefsson S., Liusvaara I. Edwards-Curve Digital Signature Algorithm (EdDSA). RFC 8032, IETF, 2017.",
  "National Institute of Standards and Technology. Module-Lattice-Based Key-Encapsulation Mechanism Standard. FIPS 203. Gaithersburg, NIST, 2024.",
  "Biryukov A., Dinu D., Khovratovich D., Josefsson S. Argon2 Memory-Hard Function for Password Hashing and Proof-of-Work Applications. RFC 9106, IETF, 2021.",
  "Perrin T. The Noise Protocol Framework. Revision 34, 2018. https://noiseprotocol.org/noise.html",
  "Shapiro M., Preguiça N., Baquero C., Zawirski M. Conflict-Free Replicated Data Types. Lecture Notes in Computer Science, 2011, vol. 6976, s.386–400.",
  "Donenfeld J.A. WireGuard: Next Generation Kernel Network Tunnel. Proceedings of the Network and Distributed System Security Symposium (NDSS), 2017.",
  "Petit-Huguenin M., Salgueiro G., Rosenberg J., Wing D., Mahy R., Matthews P. Session Traversal Utilities for NAT (STUN). RFC 8489, IETF, 2020.",
  "Maymounkov P., Mazières D. Kademlia: A Peer-to-Peer Information System Based on the XOR Metric. Lecture Notes in Computer Science, 2002, vol. 2429, s.53–65.",
  "Bernstein D.J., van Gastel B., Janssen W., Lange T., Schwabe P., Smetsers S. TweetNaCl: A Crypto Library in 100 Tweets. Lecture Notes in Computer Science, 2015, vol. 8895, s.64–83.",
];

const refParas = refs.map((r, i) => new Paragraph({
  alignment: AlignmentType.LEFT,
  indent: { left: 567, hanging: 340 },
  spacing: { line: 240 },
  children: [new TextRun({ text: `${i + 1}.\t`, font: FONT, size: 22 }), ...runs(r, { size: 22 })],
  tabStops: [{ type: TabStopType.LEFT, position: 567 }],
}));

const header = new Header({
  children: [
    P(`${SHORT} / Proceedings [№] / 2026, [səhifələr]; DOI: [redaksiya tərəfindən]`, { align: AlignmentType.CENTER, indent: {}, italics: true, size: 20 }),
    P("Informatics", { align: AlignmentType.CENTER, indent: {}, italics: true, size: 20 }),
  ],
});
const footer = new Footer({
  children: [new Paragraph({ alignment: AlignmentType.CENTER,
    children: [new TextRun({ children: [PageNumber.CURRENT], font: FONT, size: 22 })] })],
});

const doc = new Document({
  creator: SHORT,
  title: TITLE_AZ,
  styles: { default: { document: { run: { font: FONT, size: 24 } } } },
  sections: [{
    properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1134, bottom: 1134, left: 1134, right: 1134, header: 567, footer: 567 } } },
    headers: { default: header },
    footers: { default: footer },
    children: [
      center(TITLE_AZ, { bold: true }),
      blank(),
      center(AUTHOR_AZ, { bold: true }),
      center("[İş yeri], Bakı, Azərbaycan", { italics: true }),
      blank(),
      left(TITLE_EN, { bold: true }),
      blank(),
      left(AUTHOR_EN, { bold: true, size: 22 }),
      left("[Affiliation], Baku, Azerbaijan", { italics: true, size: 22 }),
      left("[e-mail]", { italics: true, size: 22 }),
      left("https://orcid.org/[ORCID]", { italics: true, size: 22 }),
      blank(),
      P(ABSTRACT_EN, { size: 22 }),
      P(KEYWORDS_EN, { size: 22 }),
      blank(),
      ...body,
      blank(),
      center("ƏDƏBİYYAT", { bold: true }),
      blank(),
      ...refParas,
      blank(),
      center(TITLE_AZ, { bold: true }),
      blank(),
      center(SHORT, { bold: true }),
      blank(),
      P(XULASE, { size: 22 }),
      P(ACAR, { size: 22 }),
    ],
  }],
});

Packer.toBuffer(doc).then((buf) => {
  fs.writeFileSync(path.join(DIR, "shusha_meqale.docx"), buf);
  console.log("wrote shusha_meqale.docx");
});

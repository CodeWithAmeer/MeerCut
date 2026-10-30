"""Build the dependency-free bilingual GitHub Pages site: python tools/build_website.py."""
from pathlib import Path
from html import escape
import json

ROOT = Path(__file__).resolve().parents[1]
REPO = 'https://github.com/CodeWithAmeer/MeerCut'
RELEASE = REPO + '/releases/tag/v1.0.0'
DOWNLOAD = REPO + '/releases/download/v1.0.0/'
LIVE = 'https://codewithameer.github.io/MeerCut/'
COPY = {
 'en': {
  'title':'MeerCut — Free, open-source video editor for Windows',
  'description':'Edit video on your own terms. MeerCut is a free, open-source Windows video editor with multitrack editing, titles, colour tools and local AI. Made by CodeWithAmeer.',
  'skip':'Skip to content', 'nav':['Features','Inside the editor','Open source'], 'download':'Download',
  'eyebrow':'Free & open source / Windows 11', 'headline':'Your footage.<br><span>Your final cut.</span>',
  'intro':'A video editor for Windows, with a multitrack timeline, colour tools, and AI that runs on your own PC.',
  'download_windows':'Download for Windows', 'github':'View on GitHub', 'release_line':'Version 1.0.0 · Windows 11, 64-bit · Free forever under the GPL',
  'frame':'THE MEERCUT WORKSPACE', 'caption':'The real editor. A short film built from MeerCut’s included sample videos.', 'full_image':'View full screenshot',
  'principles':[('No watermark','Your export stays yours.'),('No account','Download. Open. Edit.'),('No telemetry','Work without tracking.')],
  'feature_kicker':'01 / The essentials, and more', 'feature_heading':'From the first clip<br>to the last frame.',
  'feature_intro':'Cut a sequence, shape its sound, and give it a look. Familiar editing tools, together in one workspace.',
  'features':[
   ('A timeline that works with you','Trim, split, snap and ripple delete across multiple tracks. Keyframes, masks, unlimited undo, autosave and crash recovery keep the edit moving.'),
   ('Titles with character','Start with 54 title templates and 36 text animations. Style captions, import and export SRT or ASS subtitles, and work with shaped Arabic and right-to-left text.'),
   ('Colour, texture and transitions','Use HSL, curves, colour wheels and .cube LUTs. Build your look with 30 effects, 46 filters and 19 transitions.'),
   ('Sound in its own space','Mix tracks with volume automation, crossfades, a 10-band EQ, auto ducking, noise reduction and loudness normalisation.'),
   ('AI, on your computer','Remove backgrounds, create captions from speech, reframe, interpolate and upscale. Optional models download when you choose them and run locally.'),
   ('An export for the next step','Export MP4, MOV, MKV, WebM, GIF, WAV or FLAC. Use NVIDIA or Intel hardware encoding when available, with a reported CPU fallback.')],
  'gallery_kicker':'02 / Inside MeerCut', 'gallery_heading':'See what you’re working with.',
  'gallery_intro':'Actual screenshots from version 1.0.0. Explore the tools in context, from titles to local background removal.',
  'gallery':[
   ('titles','Titles','readme_02_titles.png','Titles, your way.','Browse 54 templates and adjust type, colour, spacing and alignment in the inspector.','MeerCut title template browser and text inspector, with The Journey title selected on a multitrack timeline.'),
   ('transitions','Transitions','readme_03_transitions.png','Make the cut flow.','Choose from 19 transitions and control their duration, position and direction.','MeerCut transition library with a Circle Open transition between two clips and its controls.'),
   ('colour','Colour & effects','readme_05_filters.png','Find the right look.','Apply a colour preset, then refine the image with MeerCut’s colour tools.','MeerCut filters panel applying a golden-hour look to a dragon scene.'),
   ('ai','Local AI','readme_08_background_removal.png','A different background.','Separate people from their background using local Robust Video Matting. Models are optional downloads.','MeerCut AI background removal showing two people composited over another clip.')],
  'open_kicker':'03 / Open by design', 'open_heading':'Free to use.<br>Open to build on.',
  'open_copy':'MeerCut is an independent project by CodeWithAmeer. Read the code, report an issue, or build your own version. The source and release notes are public on GitHub.',
  'source':'Explore the source', 'creator':'Creator', 'licence':'Licence', 'licence_value':'GNU GPL v3 or later', 'development':'Development', 'development_value':'Build guide',
  'download_kicker':'04 / Get MeerCut', 'download_heading':'Ready when you are.', 'download_intro':'Choose the installer, or keep it portable. Both include the editor. No additional installation is needed for the portable version.',
  'release_label':'First public release', 'installer':'Download installer', 'portable':'Portable ZIP', 'size':'Installer: 74.4 MB · Portable: 96.3 MB',
  'notes':'Release notes', 'checksums':'SHA-256 checksums', 'source_zip':'Source ZIP',
  'unsigned':'The downloads are not code-signed. Windows may display a SmartScreen notice. Verify your download against the release’s SHA-256 checksums before running it.',
  'requirements':[
   ('System','Windows 11, 64-bit. Windows 10 (1809+) is accepted by the installer but untested.'),
   ('Storage','About 200 MB for the program, plus space for media and optional AI models.'),
   ('Graphics','A DirectX 11 GPU is optional. CPU rendering is available. AI uses DirectML or Vulkan when supported, or the slower CPU fallback.')],
  'tested':'Tested on one laptop: Intel Core i5-12450H, 8 GB RAM, Intel UHD + NVIDIA RTX 2050 (4 GB), Windows 11 Home. This is the test setup, not a published minimum specification.',
  'limitations':'Before you download: current limitations',
  'limitations_copy':'Version 1.0.0 has no macOS or Linux build, no HDR or alpha-channel video export, and no AMD AMF encoder. It has not been tested on a clean Windows installation or other hardware. AI upscaling can be slow, and model licences vary; the commercial-use status of some weights is unclear.',
  'all_limits':'Read all known limitations', 'models':'AI model details and licences',
  'footer_links':['GitHub','Report an issue','Licence'], 'made':'Made by',
  'credit':'Screenshots show edited excerpts of <i>Sintel</i> and <i>Tears of Steel</i>, © Blender Foundation, under <a href="https://creativecommons.org/licenses/by/3.0/">CC BY 3.0</a>. Sources: <a href="https://durian.blender.org/">durian.blender.org</a> and <a href="https://mango.blender.org/">mango.blender.org</a>. MeerCut is not affiliated with the Blender Foundation.',
  'credit_more':'Full credits', 'hero_alt':'MeerCut editor showing a multitrack video project with titles, transitions, an effect, a sticker and audio waveforms.',
 },
 'ar': {
  'title':'MeerCut — محرر فيديو مجاني ومفتوح المصدر لويندوز',
  'description':'حرّر فيديوهاتك مع MeerCut، محرر فيديو مجاني ومفتوح المصدر لويندوز، مع مسارات متعددة وعناوين وأدوات ألوان وذكاء اصطناعي يعمل محليًا. من تطوير CodeWithAmeer.',
  'skip':'انتقل إلى المحتوى', 'nav':['المزايا','داخل المحرر','المصدر المفتوح'], 'download':'تنزيل',
  'eyebrow':'مجاني ومفتوح المصدر / ويندوز 11', 'headline':'لقطاتك.<br><span>بلمستك.</span>',
  'intro':'محرر فيديو لويندوز، يجمع خطًا زمنيًا متعدد المسارات وأدوات للألوان وذكاءً اصطناعيًا يعمل على جهازك.',
  'download_windows':'تنزيل لويندوز', 'github':'المشروع على GitHub', 'release_line':'الإصدار 1.0.0 · ويندوز 11، ‏64 بت · مجاني بموجب رخصة GPL',
  'frame':'مساحة العمل في MEERCUT', 'caption':'المحرر الفعلي، أثناء تحرير فيلم قصير من مقاطع الفيديو المرفقة مع MeerCut.', 'full_image':'عرض اللقطة كاملة',
  'principles':[('بلا علامة مائية','فيديوهاتك تحمل لمستك.'),('بلا حساب','نزّل البرنامج وابدأ التحرير.'),('بلا تتبّع','لا يجمع بيانات الاستخدام.')],
  'feature_kicker':'01 / أدوات التحرير وما بعدها', 'feature_heading':'من أول لقطة<br>إلى آخر إطار.',
  'feature_intro':'رتّب لقطاتك، واضبط الصوت، واختر الألوان. أدوات تحرير مألوفة تجتمع في مساحة عمل واحدة.',
  'features':[
   ('خط زمني يواكب عملك','قصّ المقاطع وقسّمها واحذفها مع إغلاق الفراغ عبر مسارات متعددة. إطارات مفتاحية وأقنعة وتراجع غير محدود، مع الحفظ التلقائي واستعادة العمل بعد التعطّل.'),
   ('عناوين بطابعك','ابدأ مع 54 قالبًا للعناوين و36 حركة للنص. نسّق الترجمة واستورد وصدّر ملفات SRT أو ASS، مع دعم وصل الحروف العربية والنص من اليمين إلى اليسار.'),
   ('ألوان وتأثيرات وانتقالات','استخدم HSL والمنحنيات وعجلات الألوان وملفات ‎.cube LUT. اختر من 30 تأثيرًا و46 مرشّحًا و19 انتقالًا لبناء المظهر الذي تريده.'),
   ('أدوات مخصّصة للصوت','امزج المسارات مع التحكم الآلي بمستوى الصوت والتلاشي المتداخل، وموازن من 10 نطاقات، وخفض الموسيقى تلقائيًا وتقليل الضوضاء وتسوية جهارة الصوت.'),
   ('ذكاء اصطناعي على جهازك','أزل الخلفيات، وحوّل الكلام إلى ترجمة، وأعد التأطير، وأنشئ إطارات وسيطة وارفع الدقة. تُنزَّل النماذج الاختيارية عندما تختارها وتعمل محليًا.'),
   ('تصدير يناسب خطوتك التالية','صدّر بصيغ MP4 وMOV وMKV وWebM وGIF وWAV وFLAC. استفد من ترميز NVIDIA أو Intel عند توفره، مع توضيح الانتقال إلى المعالج عند الحاجة.')],
  'gallery_kicker':'02 / داخل MeerCut', 'gallery_heading':'تعرّف على مساحة عملك.',
  'gallery_intro':'لقطات حقيقية من الإصدار 1.0.0. شاهد الأدوات أثناء العمل، من العناوين إلى إزالة الخلفية محليًا.',
  'gallery':[
   ('titles','العناوين','readme_02_titles.png','عناوين كما تريدها.','تصفّح 54 قالبًا، واضبط الخط واللون والتباعد والمحاذاة من لوحة الخصائص.','محرر MeerCut يعرض قوالب العناوين ولوحة خصائص النص، مع تحديد عنوان The Journey على خط زمني متعدد المسارات.'),
   ('transitions','الانتقالات','readme_03_transitions.png','انتقل بسلاسة بين لقطاتك.','اختر من 19 انتقالًا واضبط المدة والموضع والاتجاه.','مكتبة الانتقالات في MeerCut مع انتقال دائري بين مقطعين وإعداداته.'),
   ('colour','الألوان والتأثيرات','readme_05_filters.png','اختر المظهر المناسب.','طبّق إعدادًا لونيًا جاهزًا، ثم حسّن الصورة باستخدام أدوات الألوان في MeerCut.','لوحة المرشحات في MeerCut تطبق مظهر الساعة الذهبية على مشهد التنين.'),
   ('ai','الذكاء الاصطناعي','readme_08_background_removal.png','خلفية مختلفة.','افصل الأشخاص عن خلفياتهم محليًا باستخدام Robust Video Matting. النماذج تنزيلات اختيارية.','إزالة الخلفية بالذكاء الاصطناعي في MeerCut، مع تركيب شخصين فوق مقطع آخر.')],
  'open_kicker':'03 / مشروع مفتوح', 'open_heading':'مجاني للاستخدام.<br>ومفتوح للتطوير.',
  'open_copy':'MeerCut مشروع مستقل من تطوير CodeWithAmeer. اطّلع على الكود أو أبلغ عن مشكلة أو ابنِ نسختك الخاصة. الكود المصدري وملاحظات الإصدارات متاحان على GitHub.',
  'source':'استكشف الكود المصدري', 'creator':'المطوّر', 'licence':'الرخصة', 'licence_value':'GNU GPL، الإصدار 3 أو أحدث', 'development':'التطوير', 'development_value':'دليل بناء البرنامج',
  'download_kicker':'04 / تنزيل MeerCut', 'download_heading':'جاهز عندما تكون جاهزًا.', 'download_intro':'اختر المثبّت أو النسخة المحمولة. كلاهما يتضمن المحرر، ولا تحتاج النسخة المحمولة إلى تثبيت برامج إضافية.',
  'release_label':'أول إصدار عام', 'installer':'تنزيل المثبّت', 'portable':'النسخة المحمولة ZIP', 'size':'المثبّت: 74.4 ميغابايت · النسخة المحمولة: 96.3 ميغابايت',
  'notes':'ملاحظات الإصدار', 'checksums':'بصمات SHA-256', 'source_zip':'الكود المصدري ZIP',
  'unsigned':'ملفات التنزيل غير موقّعة رقميًا، وقد يعرض ويندوز تنبيه SmartScreen. تحقّق من مطابقة بصمة SHA-256 لملفك مع البصمة المنشورة في الإصدار قبل تشغيله.',
  'requirements':[
   ('النظام','ويندوز 11، ‏64 بت. يقبل المثبّت ويندوز 10 (1809 أو أحدث)، لكنه غير مختبَر.'),
   ('التخزين','نحو 200 ميغابايت للبرنامج، إضافة إلى مساحة للوسائط ونماذج الذكاء الاصطناعي الاختيارية.'),
   ('الرسوميات','بطاقة تدعم DirectX 11 اختيارية، ويمكن استخدام المعالج. تعمل أدوات الذكاء الاصطناعي عبر DirectML أو Vulkan عند دعمهما، أو عبر المعالج بسرعة أقل.')],
  'tested':'اختُبر على حاسوب محمول واحد: Intel Core i5-12450H، وذاكرة 8 غيغابايت، وIntel UHD مع NVIDIA RTX 2050 ‏(4 غيغابايت)، وويندوز 11 Home. هذه مواصفات جهاز الاختبار وليست حدًا أدنى معلنًا.',
  'limitations':'قبل التنزيل: القيود الحالية',
  'limitations_copy':'لا يتوفر الإصدار 1.0.0 لنظامَي macOS أو Linux، ولا يدعم تصدير HDR أو فيديو بقناة شفافية، ولا يتضمن مُرمّز AMD AMF. لم يُختبَر على تثبيت ويندوز نظيف أو أجهزة أخرى. قد يكون رفع الدقة بالذكاء الاصطناعي بطيئًا، وتختلف تراخيص النماذج؛ إذ إن حالة الاستخدام التجاري لبعض الأوزان غير واضحة.',
  'all_limits':'اقرأ جميع القيود المعروفة', 'models':'تفاصيل نماذج الذكاء الاصطناعي وتراخيصها',
  'footer_links':['GitHub','الإبلاغ عن مشكلة','الرخصة'], 'made':'من تطوير',
  'credit':'تُظهر اللقطات مقتطفات معدّلة من <i>Sintel</i> و<i>Tears of Steel</i>، © مؤسسة Blender، بموجب <a href="https://creativecommons.org/licenses/by/3.0/">CC BY 3.0</a>. المصادر: <a href="https://durian.blender.org/">durian.blender.org</a> و<a href="https://mango.blender.org/">mango.blender.org</a>. لا يرتبط MeerCut بمؤسسة Blender.',
  'credit_more':'جميع نسب الأعمال', 'hero_alt':'محرر MeerCut يعرض مشروع فيديو متعدد المسارات مع عناوين وانتقالات وتأثير وملصق وموجات صوتية.',
 }
}

DOWN = '<svg class="icon" aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M12 3v12m-5-5 5 5 5-5M5 16v5h14v-5"/></svg>'
ARROW = '<svg class="icon arrow" aria-hidden="true" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M5 12h14m-6-6 6 6-6 6"/></svg>'

def build(lang, c):
    rtl = lang == 'ar'
    p = '../' if rtl else './'
    url = LIVE + ('ar/' if rtl else '')
    other = '../' if rtl else './ar/'
    switch = 'English' if rtl else 'العربية'
    switch_lang = 'en' if rtl else 'ar'
    brand = f'<a class="brand" href="./" aria-label="MeerCut"><img src="{p}images/meercut-icon.png" width="256" height="256" alt=""><span>MeerCut</span></a>'
    features = ''.join(f'<article class="feature"><span class="number" aria-hidden="true">0{i+1}</span><h3>{title}</h3><p>{body}</p></article>' for i,(title,body) in enumerate(c['features']))
    principles = ''.join(f'<div><b>{a}</b><span>{b}</span></div>' for a,b in c['principles'])
    tabs = ''.join(f'<a data-gallery href="#{id}" aria-controls="{id}">{name}</a>' for id,name,*_ in c['gallery'])
    panels = ''.join(f'<figure class="gallery-panel" id="{id}"><a class="gallery-image" href="{p}screenshots/{img}" aria-label="{c["full_image"]}: {name}"><img src="{p}screenshots/{img}" alt="{alt}" width="1924" height="1055" loading="lazy" decoding="async"></a><figcaption><span><strong>{heading}</strong> {body}</span><a href="{p}screenshots/{img}">{c["full_image"]} ↗</a></figcaption></figure>' for id,name,img,heading,body,alt in c['gallery'])
    req = ''.join(f'<div><dt>{a}</dt><dd>{b}</dd></div>' for a,b in c['requirements'])
    nav = ''.join(f'<a href="#{id}">{label}</a>' for id,label in zip(['features','inside','open-source'],c['nav']))
    footer_links = ''.join(f'<a href="{href}">{label}</a>' for href,label in zip([REPO,REPO+'/issues',REPO+'/blob/v1.0.0/LICENSE'],c['footer_links']))
    schema = json.dumps({'@context':'https://schema.org','@type':'SoftwareApplication','name':'MeerCut','operatingSystem':'Windows 11 (64-bit)','applicationCategory':'MultimediaApplication','softwareVersion':'1.0.0','license':REPO+'/blob/v1.0.0/LICENSE','offers':{'@type':'Offer','price':'0','priceCurrency':'USD'},'author':{'@type':'Person','name':'CodeWithAmeer','url':'https://github.com/CodeWithAmeer'}},ensure_ascii=False)
    return f'''<!doctype html>
<html lang="{lang}" dir="{'rtl' if rtl else 'ltr'}">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{c['title']}</title><meta name="description" content="{escape(c['description'],quote=True)}">
<meta name="theme-color" content="#f4f2ec"><link rel="icon" type="image/png" href="{p}images/meercut-icon.png">
<link rel="canonical" href="{url}"><link rel="alternate" hreflang="en" href="{LIVE}"><link rel="alternate" hreflang="ar" href="{LIVE}ar/"><link rel="alternate" hreflang="x-default" href="{LIVE}">
<meta property="og:type" content="website"><meta property="og:title" content="{c['title']}"><meta property="og:description" content="{escape(c['description'],quote=True)}"><meta property="og:url" content="{url}"><meta property="og:image" content="{LIVE}screenshots/readme_01_editor.png"><meta property="og:locale" content="{'ar_AR' if rtl else 'en_US'}"><meta name="twitter:card" content="summary_large_image">
<link rel="stylesheet" href="{p}assets/site.css"><script src="{p}assets/site.js" defer></script><script type="application/ld+json">{schema}</script>
</head>
<body>
<a class="skip" href="#main">{c['skip']}</a>
<header class="header"><nav class="nav wrap" aria-label="{'التنقل الرئيسي' if rtl else 'Main navigation'}">{brand}<div class="nav-links">{nav}</div><div class="nav-actions"><a class="language" href="{other}" lang="{switch_lang}" hreflang="{switch_lang}" dir="{'ltr' if rtl else 'rtl'}">{switch}</a><a class="nav-download" href="#download">{c['download']}</a></div></nav></header>
<main id="main">
<section class="hero wrap" aria-labelledby="hero-title"><div class="eyebrow">{c['eyebrow']}</div><div class="hero-intro"><h1 id="hero-title">{c['headline']}</h1><div class="hero-copy"><p>{c['intro']}</p><div class="actions"><a class="button button-primary" href="{DOWNLOAD}MeerCut-1.0.0-win64-setup.exe">{DOWN}{c['download_windows']}</a><a class="text-link" href="{REPO}">{c['github']}{ARROW}</a></div><div class="micro">{c['release_line']}</div></div></div>
<figure class="editor-frame"><div class="frame-bar" aria-hidden="true"><span>{c['frame']}</span><span>01 / EDITOR</span></div><a class="editor-link" href="{p}screenshots/readme_01_editor.png" aria-label="{c['full_image']}"><img src="{p}screenshots/readme_01_editor.png" width="1924" height="1055" alt="{c['hero_alt']}" fetchpriority="high"></a></figure>
<div class="hero-caption"><span>{c['caption']}</span><a href="{p}screenshots/readme_01_editor.png">{c['full_image']} ↗</a></div><div class="principles">{principles}</div></section>
<section class="section wrap" id="features" aria-labelledby="features-title"><div class="section-head"><div><p class="kicker">{c['feature_kicker']}</p><h2 id="features-title">{c['feature_heading']}</h2></div><p>{c['feature_intro']}</p></div><div class="feature-grid">{features}</div></section>
<section class="section explore" id="inside" aria-labelledby="inside-title"><div class="wrap"><div class="section-head"><div><p class="kicker">{c['gallery_kicker']}</p><h2 id="inside-title">{c['gallery_heading']}</h2></div><p>{c['gallery_intro']}</p></div><nav class="gallery-tabs" aria-label="{'اختر لقطة شاشة' if rtl else 'Choose a screenshot'}">{tabs}</nav>{panels}</div></section>
<section class="open-section wrap" id="open-source" aria-labelledby="open-title"><div><p class="kicker">{c['open_kicker']}</p><h2 id="open-title">{c['open_heading']}</h2><p>{c['open_copy']}</p><a class="text-link" href="{REPO}">{c['source']}{ARROW}</a></div><dl class="open-facts"><div><dt>{c['creator']}</dt><dd><a href="https://github.com/CodeWithAmeer"><bdi>CodeWithAmeer</bdi></a></dd></div><div><dt>{c['licence']}</dt><dd><a href="{REPO}/blob/v1.0.0/LICENSE">{c['licence_value']}</a></dd></div><div><dt>{c['development']}</dt><dd><a href="{REPO}/blob/v1.0.0/BUILD.md">{c['development_value']}</a></dd></div></dl></section>
<section class="section download" id="download" aria-labelledby="download-title"><div class="wrap"><div class="section-head"><div><p class="kicker">{c['download_kicker']}</p><h2 id="download-title">{c['download_heading']}</h2></div><p>{c['download_intro']}</p></div><div class="download-grid"><div><div class="package"><div class="release"><b dir="ltr">MeerCut 1.0.0</b><span>{c['release_label']}</span></div><div class="actions"><a class="button button-primary" href="{DOWNLOAD}MeerCut-1.0.0-win64-setup.exe">{DOWN}{c['installer']}</a><a class="button button-secondary" href="{DOWNLOAD}MeerCut-1.0.0-win64-portable.zip">{c['portable']}{ARROW}</a></div><p class="micro">{c['size']}</p></div><div class="download-links"><a href="{RELEASE}">{c['notes']}</a><a href="{DOWNLOAD}SHA256SUMS.txt">{c['checksums']}</a><a href="{DOWNLOAD}MeerCut-1.0.0-source.zip">{c['source_zip']}</a></div><p class="download-note">{c['unsigned']}</p></div><div><dl class="requirements">{req}</dl><p class="download-note">{c['tested']}</p></div></div><details><summary>{c['limitations']}</summary><p>{c['limitations_copy']}</p><div class="download-links"><a href="{REPO}#known-limitations">{c['all_limits']}</a><a href="{REPO}/blob/v1.0.0/AI_MODELS.md">{c['models']}</a></div></details></div></section>
</main>
<footer class="footer wrap"><div class="footer-top">{brand}<div class="footer-links">{footer_links}</div></div><div class="footer-bottom"><p class="copyright">© 2026 MeerCut. {c['made']} <a href="https://github.com/CodeWithAmeer"><bdi>CodeWithAmeer</bdi></a>.<br><span dir="ltr">GPL-3.0-or-later</span></p><p>{c['credit']} <a href="{REPO}/blob/v1.0.0/CREDITS.md">{c['credit_more']}</a>.</p></div></footer>
</body></html>
'''

if __name__ == '__main__':
    for lang, copy in COPY.items():
        folder = ROOT / 'docs' / ('ar' if lang == 'ar' else '')
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'index.html').write_text(build(lang, copy), encoding='utf-8')
    (ROOT / 'docs' / '.nojekyll').touch()
    print('Built docs/index.html and docs/ar/index.html')

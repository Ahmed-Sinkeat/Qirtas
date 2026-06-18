<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="src/ui/icons/qirtas-logo-dark.png">
  <img src="src/ui/icons/qirtas-logo.png" width="150" alt="Qirtas">
</picture>

# قِرطاس · Qirtas

محرّر ملاحظات خفيف للينكس يحاول أن يبقى محرّر ملاحظات فقط.

[English](README.en.md) · **العربية**

[![أحدث إصدار](https://img.shields.io/github/v/release/Ahmed-Sinkeat/Qirtas?display_name=tag&label=%D8%A7%D9%84%D8%A5%D8%B5%D8%AF%D8%A7%D8%B1)](https://github.com/Ahmed-Sinkeat/Qirtas/releases/latest)

<sub>نواة Zig · واجهة GTK4 / Libadwaita · بلا Electron · بلا حسابات · بلا ذكاء اصطناعي داخل المحرّر</sub>

</div>

---

<div dir="rtl" align="right">


## ما هو قرطاس؟

برنامج ملاحظات خفيف يركز على الكتابة والقراءة والتنظيم.

تكتب فيه ملاحظاتك، تحفظها بصيغة Markdown عادية، وتنقلها أينما شئت. لا اشتراك، ولا حساب، ولا خدمة تملك بياناتك نيابة عنك.

إذا أردت المزامنة، استخدم الخدمة التي تثق بها. إذا أردت نسخة احتياطية، فالملفات موجودة لديك. وإذا قررت ترك قرطاس غداً، فلن تحتاج إلى تصدير بياناتك من صيغة خاصة أو منصة مغلقة.


## لقطات

</div>

<div align="center">

| المحرّر | وضع التركيز |
|:---:|:---:|
| ![المحرّر](assets/screenshots/editor.png) | ![وضع القراءة](assets/screenshots/read-mode.png) |
| **سمات الورق والحبر** | **العربية من اليمين إلى اليسار** |
| ![السمات](assets/screenshots/themes.png) | ![عربية](assets/screenshots/arabic-rtl.png) |

</div>

<div dir="rtl" align="right">

## التثبيت

### AppImage — الأسهل، يعمل على أي توزيعة بلا تثبيت

١. نزّل ملف `Qirtas-x86_64.AppImage` من صفحة [الإصدارات](https://github.com/Ahmed-Sinkeat/Qirtas/releases).

٢. اجعله قابلاً للتشغيل:

```sh
chmod +x Qirtas-x86_64.AppImage
```

٣. شغّله:

```sh
./Qirtas-x86_64.AppImage
```

بلا صلاحيات جذر، ولا شيء يُنسخ إلى نظامك. (يحتاج FUSE، وهو موجود في أغلب التوزيعات.)

### آرتش لينكس

ابنِ الحزمة من المستودع مباشرة (لا يحتاج حساب AUR):

```sh
git clone https://github.com/Ahmed-Sinkeat/Qirtas.git
cd Qirtas/packaging
makepkg -si
```

أو ثبّت الحزمة الجاهزة من صفحة [الإصدارات](https://github.com/Ahmed-Sinkeat/Qirtas/releases):

```sh
sudo pacman -U qirtas-1.0.0-1-x86_64.pkg.tar.zst
```

> `yay -S qirtas-git` و`paru -S qirtas-git` سيعملان بعد نشر الحزمة على AUR.

### من المصدر

تحتاج إلى Zig 0.16، وGTK4، وLibadwaita، وGtkSourceView 5، وSQLite. ثبّت المتطلبات حسب توزيعتك:

```sh
# آرتش
sudo pacman -S --needed zig gtk4 gtksourceview5 libadwaita sqlite

# فيدورا
sudo dnf install zig gtk4-devel gtksourceview5-devel libadwaita-devel sqlite-devel

# دبيان / أوبنتو (إن كانت Zig في المستودعات أقدم من 0.16، نزّلها من ziglang.org)
sudo apt install libgtk-4-dev libgtksourceview-5-dev libadwaita-1-dev libsqlite3-dev
```

ثم ابنِ وشغّل:

```sh
git clone https://github.com/Ahmed-Sinkeat/Qirtas.git
cd Qirtas
zig build run
```

ملاحظاتك وإعداداتك تعيش في `~/.config/qirtas/`.



## الحالة الحالية

قرطاس ما زال قبل الإصدار 1.0، يكتبه ويصونه شخص واحد، ويُستخدم يومياً لتدوين
ملاحظات حقيقية. هو يعمل، لكن توقّع بعض الحواف الخشنة. ولأن ملاحظاتك ملفات `.md`
عادية، فأسوأ الأحوال يبقى مجرّد Markdown على قرصك. المشكلات المعروفة مدوّنة بصراحة
في [docs/ISSUES.md](docs/ISSUES.md).

## ما أنوي إضافته

* تطوير دعم العربية.
* تحسين تجربة الكتابة.
* تقسيم الشاشة.
* تحسين التصدير والقوالب.
* استيراد وتصدير DOCX.
* تصحيح تلقائي للأخطاء
* نسخة للهواتف.
* نظام إضافات.

## ما لا أنوي إضافته إلى النواة الأساسية

* أي ميزة لا تحسّن تجربة الكتابة أو القراءة أو تنظيم الملاحظات بشكل مباشر.

## المساهمة

ابدأ بقراءة [docs/STRUCTURE.md](docs/STRUCTURE.md) — يشرح كيف يُبنى المشروع وأين
يعيش كل شيء (Zig يملك نصّ المستند، وطرف C/GTK يرسمه).

ثم يبقى سؤال واحد يُطرح قبل أي ميزة:

> **هل تجعل هذه الميزة قرطاس برنامج ملاحظات أفضل؟**
>
> إذا كانت الإجابة نعم، فقد تدخل إلى المشروع.
> إذا كانت الإجابة لا، فإمّا أن تكون إضافة مستقلة أو لا تُضاف إطلاقاً.

هذا السؤال هو ما يُبقي برنامج الملاحظات برنامج ملاحظات. افتح مسألة (issue) قبل أي
تغيير كبير حتى نزنه بهذا السؤال معاً.

</div>

---

<div align="center"><sub>رخصة GPL-3.0 · صُنع للكتابة، لا للمنصّات</sub></div>

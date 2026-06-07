# Qirtas — Theme Files / ملفات الثيمات

هذا المجلد يحتوي على ملفات CSS الخاصة بكل ثيم بشكل مستقل.

## الملفات / Files

| الملف | الثيم |
|---|---|
| `theme-dark.css`     | Deep Slate Dark (الافتراضي) |
| `theme-sepia.css`    | Classic Sepia Light |
| `theme-midnight.css` | Midnight Pure Black |

## كيفية تخصيص الثيم

1. افتح الملف المناسب للثيم الذي تريد تعديله
2. غيّر قيم الألوان (موجودة في تعليق الـ "Color Tokens" في أعلى كل ملف)
3. احفظ الملف — عند تشغيل التطبيق في المرة القادمة سيتم تحميل التغييرات تلقائياً

## كيفية إضافة ثيم جديد

1. انسخ `theme-dark.css` باسم جديد مثل `theme-ocean.css`
2. غيّر الألوان كما تشاء
3. في `src/gui.c` — في دالة `apply_theme()` — أضف شرطاً جديداً:
   ```c
   } else if (strcmp(theme_name, "ocean") == 0) {
       theme_css_path = "src/ui/themes/theme-ocean.css";
       gutter_color   = "#4fc3f7";
       active_num_color = "#00e5ff";
   ```
4. أضف زر الثيم الجديد في صفحة الإعدادات داخل `gui.c`

## آلية التحميل / Loading Mechanism

```
apply_theme(gui, "dark")
  └─► يبحث عن src/ui/themes/theme-dark.css
      ├── إذا وجده  → يحمّله ويطبّقه ✅
      └── إذا لم يجده → يرجع للـ CSS المدمج (fallback) ⚠️
```

يضمن هذا التصميم أن التطبيق **لن يتعطل** حتى لو حُذف ملف الثيم.

## بنية CSS / Structure

كل ملف ثيم منظّم بأقسام:

```
1. Global (font, window bg)
2. Sidebar
3. Navigation Buttons
4. Workspace / Editor
5. Search Bar
6. File Tree Explorer
7. Bottom Bar
8. Sync Cards / Settings
9. Scrollbar
```

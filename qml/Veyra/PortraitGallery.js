.pragma library

var files = ["dawn-ridge.jpg","blue-hour.jpg","tidal-light.jpg","desert-dusk.jpg"]

var translations = {
    "zh-CN": {
        "title": "两侧背景",
        "black": "黑色",
        "builtin": "内置",
        "custom": "自定义",
        "slideshow": "幻灯片",
        "interval": "每 20 秒随机更换左右两侧的竖图。",
        "preview": "左右预览",
        "select": "点击左图或右图，为这一侧选图。",
        "left": "左侧",
        "right": "右侧",
        "brightness": "背景图亮度",
        "images": "图片"
    },
    "zh-TW": {
        "title": "兩側背景",
        "black": "黑色",
        "builtin": "內建",
        "custom": "自訂",
        "slideshow": "投影片",
        "interval": "每 20 秒隨機更換左右兩側的直式圖片。",
        "preview": "左右預覽",
        "select": "點選左圖或右圖，為該側選圖。",
        "left": "左側",
        "right": "右側",
        "brightness": "背景圖亮度",
        "images": "圖片"
    },
    "en": {
        "title": "Side backgrounds",
        "black": "Black",
        "builtin": "Built-in",
        "custom": "Custom",
        "slideshow": "Slideshow",
        "interval": "Both sides change to random portraits every 20 seconds.",
        "preview": "Side previews",
        "select": "Select a preview to choose the image for that side.",
        "left": "Left",
        "right": "Right",
        "brightness": "Background brightness",
        "images": "Images"
    },
    "ko": {
        "title": "측면 배경",
        "black": "검정",
        "builtin": "기본 이미지",
        "custom": "사용자 지정",
        "slideshow": "슬라이드 쇼",
        "interval": "20초마다 양쪽 세로 이미지가 무작위로 바뀝니다.",
        "preview": "좌우 미리보기",
        "select": "미리보기를 눌러 해당 측면의 이미지를 선택합니다.",
        "left": "왼쪽",
        "right": "오른쪽",
        "brightness": "배경 밝기",
        "images": "이미지"
    },
    "ja": {
        "title": "左右の背景",
        "black": "黒",
        "builtin": "内蔵",
        "custom": "カスタム",
        "slideshow": "スライドショー",
        "interval": "20 秒ごとに左右の縦長画像をランダムに切り替えます。",
        "preview": "プレビュー",
        "select": "左右のプレビューを選んで画像を変更します。",
        "left": "左側",
        "right": "右側",
        "brightness": "背景の明るさ",
        "images": "画像"
    },
    "ru": {
        "title": "Фон по бокам",
        "black": "Чёрный",
        "builtin": "Встроенный",
        "custom": "Свой",
        "slideshow": "Слайд-шоу",
        "interval": "Каждые 20 секунд картинки по бокам меняются на случайные.",
        "preview": "Превью сторон",
        "select": "Нажмите на левое или правое превью, чтобы выбрать картинку для этой стороны.",
        "left": "Слева",
        "right": "Справа",
        "brightness": "Яркость фона",
        "images": "Картинки"
    },
    "de": {
        "title": "Seitenhintergründe",
        "black": "Schwarz",
        "builtin": "Integriert",
        "custom": "Eigene",
        "slideshow": "Diashow",
        "interval": "Beide Seiten wechseln alle 20 Sekunden zu zufälligen Hochformatbildern.",
        "preview": "Seitenvorschau",
        "select": "Wählen Sie eine Vorschau, um das Bild für diese Seite festzulegen.",
        "left": "Links",
        "right": "Rechts",
        "brightness": "Hintergrundhelligkeit",
        "images": "Bilder"
    },
    "es": {
        "title": "Fondos laterales",
        "black": "Negro",
        "builtin": "Integrado",
        "custom": "Personalizado",
        "slideshow": "Presentación",
        "interval": "Ambos lados cambian a imágenes verticales aleatorias cada 20 segundos.",
        "preview": "Vista previa de los lados",
        "select": "Selecciona una vista previa para elegir la imagen de ese lado.",
        "left": "Izquierda",
        "right": "Derecha",
        "brightness": "Brillo del fondo",
        "images": "Imágenes"
    },
    "fr": {
        "title": "Arrière-plans latéraux",
        "black": "Noir",
        "builtin": "Intégré",
        "custom": "Personnalisé",
        "slideshow": "Diaporama",
        "interval": "Les deux côtés passent à des images verticales aléatoires toutes les 20 secondes.",
        "preview": "Aperçu des côtés",
        "select": "Sélectionnez un aperçu pour choisir l’image de ce côté.",
        "left": "Gauche",
        "right": "Droite",
        "brightness": "Luminosité de l’arrière-plan",
        "images": "Images"
    },
    "pt-BR": {
        "title": "Fundos laterais",
        "black": "Preto",
        "builtin": "Integrado",
        "custom": "Personalizado",
        "slideshow": "Apresentação",
        "interval": "Os dois lados mudam para imagens verticais aleatórias a cada 20 segundos.",
        "preview": "Prévia dos lados",
        "select": "Selecione uma prévia para escolher a imagem desse lado.",
        "left": "Esquerda",
        "right": "Direita",
        "brightness": "Brilho do fundo",
        "images": "Imagens"
    }
}

function words(locale) {
    return translations[locale] || translations[locale.split("-")[0]] || translations["zh-CN"]
}

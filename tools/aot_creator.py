import os
import base64

print("=========================================")
print("   INICIANDO FÁBRICA AOT - NGBC iOS      ")
print("=========================================")

os.makedirs("src/converted_assets", exist_ok=True)

game_file = "game.xex"
if os.path.exists(game_file):
    file_size = os.path.getsize(game_file) / (1024 * 1024)
    print(f"Arquivo encontrado: {game_file} ({file_size:.2f} MB)")
    
    with open(game_file, "rb") as f:
        header = f.read(4)
        if header == b'XEX2':
            print("Assinatura XEX2 válida detectada! Este é um executável do Xbox 360.")
        else:
            print(f"Aviso: Assinatura desconhecida: {header.hex()}")
else:
    print("Erro: Arquivo game.xex não encontrado no runner.")

# Gerando a textura de teste provisória (até extrairmos os sprites reais)
png_data = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAABGdBTUEAALGPC/xhBQAAACBjSFJN"
    "AAB6JgAAgIQAAPoAAACA6AAAdTAAAOpgAAA6mAAAF3CculE8AAAABmJLR0QA/wD/AP+gvaeTAAAA"
    "VElEQVR42u3BAQ0AAADCoPdPbQ8HFAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA4G8wAAABJgH6VQAAAABJRU5ErkJggg=="
)

with open("src/converted_assets/texture.png", "wb") as f:
    f.write(png_data)

print("[1/1] Textura provisória gerada.")
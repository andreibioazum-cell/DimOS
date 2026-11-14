#!/bin/bash
# Скрипт для конвертации изображений в формат TGA
# Использование: ./convert_to_tga.sh [input_file]
# Если файл не указан, конвертируются все PNG, JPG, BMP в папке

INPUT_FILE="$1"

if [ -n "$INPUT_FILE" ]; then
    # Конвертируем указанный файл
    if [ -f "$INPUT_FILE" ]; then
        OUTPUT_FILE="${INPUT_FILE%.*}.tga"
        echo "Конвертируем $INPUT_FILE в $OUTPUT_FILE..."
        convert "$INPUT_FILE" "$OUTPUT_FILE"
        if [ $? -eq 0 ]; then
            echo "Готово!"
        else
            echo "Ошибка конвертации"
            exit 1
        fi
    else
        echo "Файл $INPUT_FILE не найден"
        exit 1
    fi
else
    # Конвертируем все поддерживаемые форматы в папке
    echo "Конвертируем все изображения в папке в формат TGA..."
    for file in *.png *.jpg *.jpeg *.bmp *.gif *.webp; do
        if [ -f "$file" ]; then
            output="${file%.*}.tga"
            echo "  $file -> $output"
            convert "$file" "$output"
            if [ $? -ne 0 ]; then
                echo "  Ошибка конвертации $file"
            fi
        fi
    done
    echo "Готово!"
fi

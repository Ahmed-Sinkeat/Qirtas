import os

def generate_file(filename, lines_count):
    with open(filename, "w") as f:
        for i in range(1, lines_count + 1):
            if i % 100 == 1:
                f.write(f"# Heading {i}\n")
            elif i % 100 == 50:
                f.write("Some text with a [[WikiLink]] inside it.\n")
            elif i % 100 == 75:
                f.write("---\n")
            else:
                f.write(f"This is line {i} of the markdown file with some random words.\n")

generate_file("test_1000.md", 1000)
generate_file("test_3000.md", 3000)
generate_file("test_5000.md", 5000)
print("Files generated successfully.")

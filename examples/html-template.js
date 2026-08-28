console.log("=== Mystral Lexbor HTML Template Test ===");

const template = document.createElement("template");
template.innerHTML = `<section class="panel"><h1>Mystral</h1><!-- template marker --></section>tail`;

const content = template.content;
const section = content.firstChild;
const heading = section?.firstChild;
const comment = heading?.nextSibling;
const tail = section?.nextSibling;
const clone = content.cloneNode(true);

const passed =
    content?.nodeType === 11 &&
    section?.tagName === "SECTION" &&
    section?.className === "panel" &&
    heading?.tagName === "H1" &&
    heading?.firstChild?.data === "Mystral" &&
    comment?.nodeType === 8 &&
    tail?.nodeType === 3 &&
    clone !== content &&
    clone.firstChild !== section &&
    clone.firstChild?.parentNode === clone;

console.log(`HTML_TEMPLATE_NODES=${content.childNodes.length}`);
console.log(`HTML_TEMPLATE_RESULT=${passed ? "pass" : "fail"}`);
process.exit(passed ? 0 : 1);
